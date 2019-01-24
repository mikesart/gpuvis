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
#ifndef TDOPEXPR_H_
#define TDOPEXPR_H_

typedef std::function< const char * ( const char *name, size_t len ) > tdop_get_key_func;
typedef std::function< const char * ( const char *name, char ( &buf )[ 64 ] ) > tdop_get_keyval_func;

class TdopExpr *tdopexpr_compile( const char *expression, tdop_get_key_func &get_key_func, std::string &errstr );
const char *tdopexpr_exec( class TdopExpr *tdop_expr, tdop_get_keyval_func &get_keyval_func );
void tdopexpr_delete( class TdopExpr *tdop_expr );

#endif // TDOPEXPR_H_
