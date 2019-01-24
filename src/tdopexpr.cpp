/*
 * Copyright 2019 Valve Software
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

#include <future>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <string>

#include "tdopexpr.h"
#include "gpuvis_macros.h"

// TDOP: "Top down operator precedence parsing"
//   http://eli.thegreenplace.net/2010/01/02/top-down-operator-precedence-parsing
//   http://effbot.org/zone/simple-top-down-parsing.htm

enum tdop_tok_type_t
{
    TOK_NULL,
    TOK_ERROR,
    TOK_END,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_NUMBER,
    TOK_STRING,
    TOK_VARIABLE,
    TOK_INFIX_OP
};

typedef const char * ( TDOP_INFIX_FUNC )( const char *a, const char *b );

struct tdop_state_token
{
    int lbp;
    tdop_tok_type_t type;

    const char *variable;
    TDOP_INFIX_FUNC *function;
    char value_buf[ 64 ];

    void set_value_buf( const char *val, size_t val_len )
    {
        size_t len = std::min< size_t >( sizeof( value_buf ) - 1, val_len );

        memcpy( value_buf, val, len );
        value_buf[ len ] = 0;
    }
};

struct tdop_state
{
    tdop_state_token tok;

    const char *start;
    const char *next;

    tdop_get_key_func get_key_func;
};

static int num_compare( const char *a, const char *b, int defval )
{
    char *endptr;

    if ( !a[ 0 ] || !b[ 0 ] )
        return defval;

    if ( a[ 0 ] == '-' || b[ 0 ] == '-' ||
         strchr( a, '.' ) || strchr( b, '.' ) )
    {
        double val_a = strtod( a, &endptr );
        double val_b = strtod( b, &endptr );

        if ( val_a == val_b )
            return 0;
        else if ( val_a < val_b )
            return -1;
        return 1;
    }
    else
    {
        int base_a = ( a[ 0 ] == '0' && a[ 1 ] == 'x' ) ? 16 : 10;
        int base_b = ( b[ 0 ] == '0' && b[ 1 ] == 'x' ) ? 16 : 10;

        uint64_t val_a = strtoull( a, &endptr, base_a );
        uint64_t val_b = strtoull( b, &endptr, base_b );

        if ( val_a == val_b )
            return 0;
        else if ( val_a < val_b )
            return -1;
        return 1;
    }
}

static const char *func_gt( const char *a, const char *b )
{
    return ( num_compare( a, b, -1 ) > 0 ) ? "1" : "";
}

static const char *func_ge( const char *a, const char *b )
{
    return ( num_compare( a, b, -1 ) >= 0 ) ? "1" : "";
}

static const char *func_lt( const char *a, const char *b )
{
    return ( num_compare( a, b, 1 ) < 0 ) ? "1" : "";
}

static const char *func_le( const char *a, const char *b )
{
    return ( num_compare( a, b, 1 ) <= 0 ) ? "1" : "";
}

static const char *func_and( const char *a, const char *b )
{
    return a[ 0 ] && b[ 0 ] ? "1" : "";
}

static const char *func_or( const char *a, const char *b )
{
    return a[ 0 ] || b[ 0 ] ? "1" : "";
}

static const char *func_equal( const char *a, const char *b )
{
    return !strcasecmp( a, b ) ? "1" : "";
}

static const char *func_notequal( const char *a, const char *b )
{
    return strcasecmp( a, b ) ? "1" : "";
}

/* $line =~ [[:space:]]* */
/* contains operator: "12345678 =~ 345" is true */
static const char *func_contains( const char * a, const char * b )
{
    return ( b[ 0 ] && strcasestr( a, b ) ) ? "1" : "";
}

static void next_token( tdop_state *s )
{
    s->tok.lbp = 0;
    s->tok.type = TOK_NULL;
    s->tok.variable = NULL;
    s->tok.function = NULL;
    s->tok.value_buf[ 0 ] = 0;

    while ( s->tok.type == TOK_NULL )
    {
        if ( !s->next[ 0 ] )
        {
            s->tok.type = TOK_END;
            return;
        }

        if ( s->next[ 0 ] == '$' )
        {
            const char *value = ++s->next;

            while ( isalpha( s->next[ 0 ] ) ||
                    isdigit( s->next[ 0 ] ) ||
                    ( s->next[ 0 ] == '_' ) )
            {
                s->next++;
            }

            s->tok.type = TOK_VARIABLE;
            s->tok.variable = s->get_key_func( value, s->next - value );
            if ( !s->tok.variable )
                s->tok.type = TOK_ERROR;
        }
        else if ( s->next[ 0 ] == '"' )
        {
            s->tok.type = TOK_STRING;
            const char *value = s->next + 1;

            const char *endquote = strchr( value, '"' );
            if ( endquote )
            {
                s->next = endquote + 1;
                s->tok.set_value_buf( value, endquote - value );
            }
            else
            {
                s->tok.type = TOK_ERROR;
            }
        }
        else if ( isalpha( s->next [ 0 ] ) )
        {
            s->tok.type = TOK_STRING;
            const char *value = s->next;

            while ( isalpha( s->next[ 0 ] ) ||
                    isdigit( s->next[ 0 ] ) ||
                    ( s->next[ 0 ] == '_' ) )
            {
                s->next++;
            }

            s->tok.set_value_buf( value, s->next - value );
        }
        else if ( s->next[ 0 ] == '0' && s->next[ 1 ] == 'x' )
        {
            char *endptr;

            s->tok.type = TOK_NUMBER;
            const char *value = s->next;

            strtoull( value, &endptr, 16 );

            s->next = endptr;
            s->tok.set_value_buf( value, endptr - value );
        }
        else if ( isdigit( s->next[ 0 ] ) ||
                  ( ( s->next[ 0 ] == '-' ) && isdigit( s->next[ 1 ] ) ) )
        {
            char *endptr;

            s->tok.type = TOK_NUMBER;
            const char *value = s->next;

            strtod( value, &endptr );

            s->next = endptr;
            s->tok.set_value_buf( value, endptr - value );
        }
        else
        {
            struct op_t
            {
                const char *opstr;
                TDOP_INFIX_FUNC *function;
                int lbp;
            };
            static const op_t s_ops[] =
            {
                { "&&", func_and, 10 },
                { "||", func_or, 10 },
                { "!=", func_notequal, 20 },
                { "=~", func_contains, 20 },
                { "==", func_equal, 20 },
                { "=", func_equal, 20 },
                { ">=", func_ge, 20 },
                { ">", func_gt, 20 },
                { "<=", func_le, 20 },
                { "<", func_lt, 20 },
            };

            const char *n = s->next++;

            switch ( n[ 0 ] )
            {
            case '(':
                s->tok.type = TOK_LPAREN;
                s->tok.lbp = 0;
                break;

            case ')':
                s->tok.type = TOK_RPAREN;
                s->tok.lbp = 0;
                break;

            case ' ':
            case '\t':
            case '\n':
            case '\r':
                s->tok.type = TOK_NULL;
                break;

            default:
                s->tok.type = TOK_ERROR;

                for ( size_t i = 0; i < ARRAY_SIZE( s_ops ); i++ )
                {
                    const op_t &op = s_ops[ i ];

                    if ( ( op.opstr[ 0 ] == n[ 0 ] ) &&
                         ( !op.opstr[ 1 ] || op.opstr[ 1 ] == n[ 1 ] ) )
                    {
                        if ( op.opstr[ 1 ] )
                            s->next++;

                        s->tok.type = TOK_INFIX_OP;
                        s->tok.lbp = op.lbp;
                        s->tok.function = op.function;
                        break;
                    }
                }
                break;
            }
        }
    }
}

class TdopExpr
{
public:
    TdopExpr() {}
    ~TdopExpr() {}

    int compile( const char *expression, tdop_get_key_func &get_key_func, std::string &errstr );
    const char *exec( tdop_get_keyval_func &get_keyval_func );

protected:
    tdop_state_token *get_next_token();
    const char *tdop_infix( tdop_state_token *self, const char *left );
    const char *tdop_expression( int rbp );

public:
    tdop_get_keyval_func m_get_keyval_func;
    tdop_state_token *m_token = nullptr;

    size_t m_token_index = 0;
    std::vector< tdop_state_token > m_vec_tokens;
};

class TdopExpr *tdopexpr_compile( const char *expression, tdop_get_key_func &get_key_func, std::string &errstr )
{
    TdopExpr *tdop_expr = new TdopExpr;

    if ( tdop_expr->compile( expression, get_key_func, errstr ) < 0 )
    {
        delete tdop_expr;
        tdop_expr = NULL;
    }

    return tdop_expr;
}

const char *tdopexpr_exec( class TdopExpr *tdop_expr, tdop_get_keyval_func &get_keyval_func )
{
    return tdop_expr ? tdop_expr->exec( get_keyval_func ) : "";
}

void tdopexpr_delete( TdopExpr *tdop_expr )
{
    delete tdop_expr;
}

tdop_state_token *TdopExpr::get_next_token()
{
    if ( m_token_index >= m_vec_tokens.size() )
        return &m_vec_tokens.back();

    return &m_vec_tokens[ m_token_index++ ];
}

const char *TdopExpr::tdop_infix( tdop_state_token *self, const char *left )
{
    const char *right = tdop_expression( self->lbp );

    return self->function( left, right );
}

const char *TdopExpr::tdop_expression( int rbp )
{
    const char *left;

    if ( m_token->type == TOK_LPAREN )
    {
        m_token = get_next_token();
        left = tdop_expression( 0 );

        // m_token should be TOK_RPAREN right now
    }
    else if ( m_token->type == TOK_VARIABLE )
    {
        left = m_get_keyval_func( m_token->variable, m_token->value_buf );
    }
    else
    {
        // m_token should be TOK_STRING / TOK_NUMBER
        left = m_token->value_buf;
    }

    m_token = get_next_token();
    // m_token should be TOK_INFIX_OP

    while ( rbp < m_token->lbp )
    {
        tdop_state_token *tok = m_token;

        m_token = get_next_token();

        left = tdop_infix( tok, left );
    }

    return left;
}

const char *TdopExpr::exec( tdop_get_keyval_func &get_keyval_func )
{
    m_get_keyval_func = get_keyval_func;

    m_token_index = 0;
    m_token = get_next_token();

    return tdop_expression( 0 );
}

static bool is_arg( tdop_tok_type_t type )
{
    return ( type == TOK_NUMBER ) ||
            ( type == TOK_STRING ) ||
            ( type == TOK_VARIABLE );
}

static std::string validate_info_tokens( std::vector< tdop_state_token > &tokens )
{
    int num_ops = 0;
    int num_parens = 0;
    tdop_tok_type_t left_type = TOK_NULL;

    //    arg op arg
    //    arg op ( arg )
    //    ( arg op arg )
    //    ( arg op arg ) op arg
    //    ( arg op arg ) op ( ( arg ) op arg )
    //    ( arg op ( arg op arg ) )

    for ( const tdop_state_token &tok : tokens )
    {
        tdop_tok_type_t type = tok.type;

        switch( type )
        {
        case TOK_NULL:
        case TOK_ERROR:
            return "ERROR: Failed parsing filter string";

        case TOK_NUMBER:
        case TOK_STRING:
        case TOK_VARIABLE:
            if ( ( left_type != TOK_INFIX_OP ) &&
                 ( left_type != TOK_LPAREN ) &&
                 ( left_type != TOK_NULL ) )
            {
                return "ERROR: Unexpected token left of arg";
            }
            break;

        case TOK_INFIX_OP:
            num_ops++;
            if ( !is_arg( left_type ) && ( left_type != TOK_RPAREN ) )
                return "ERROR: Unexpected token left of op";
            break;

        case TOK_LPAREN:
            if ( ( left_type != TOK_LPAREN ) &&
                 ( left_type != TOK_INFIX_OP ) &&
                 ( left_type != TOK_NULL ) )
            {
                return "ERROR: Unexpected token left of left paren";
            }

            num_parens++;
            break;

        case TOK_RPAREN:
            if ( --num_parens < 0 )
                return "ERROR: Unexpected right paren";
            if ( !is_arg( left_type ) && ( left_type != TOK_RPAREN ) )
                return "ERROR: Unexpected token left of right paren";
            break;

        case TOK_END:
            if ( num_parens )
                return "ERROR: Mismatched parens";
            if ( !num_ops )
                return "ERROR: No ops found";
            if ( !is_arg( left_type ) && ( left_type != TOK_RPAREN ) )
                return "ERROR: Unexpected end token";

            return "";
        }

        left_type = type;
    }

    return "ERROR: Parsing filter string failed";
}

int TdopExpr::compile( const char *expression, tdop_get_key_func &get_key_func, std::string &errstr )
{
    tdop_state s;

    s.start = s.next = expression;
    s.get_key_func = get_key_func;

    // Parse all tokens
    m_vec_tokens.clear();
    for ( ;; )
    {
        next_token( &s );

        m_vec_tokens.push_back( s.tok );

        if ( ( s.tok.type == TOK_END ) || ( s.tok.type == TOK_ERROR ) )
            break;
    }

    errstr = validate_info_tokens( m_vec_tokens );

    return errstr.empty() ? 0 : -1;
}
