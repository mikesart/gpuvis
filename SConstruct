#!python

# MSVC_VERSION values:
#   14.0      # Visual Studio 2015
#   14.0Exp   #   Express version
#   12.0      # Visual Studio 2013
#   12.0Exp
#   11.0      # Visual Studio 2012
#   11.0Exp
#   10.0      # Visual Studio 2010
#   10.0Exp
#   9.0       # Visual Studio 2008
#   9.0Exp
#   8.0       # Visual Studio 2005
#   8.0Exp
#   7.1       # Visual Studio .NET
#   7.0
#   6.0       # Visual Studio 6.0

from build_support import *

projectname = 'gpuvis'
sconfiles = {}
sconfiles[ 'src/gpuvis.scons' ] = { 'MSVC_VERSION' : '14.0' }

builddata = BuildData()
Export('builddata')

builddata.vars.Add( BoolVariable( 'gtk3', 'Build with gtk3 open file dialog', 1 ) )
builddata.vars.Add( BoolVariable( 'freetype', 'Build with freetype', 1 ) )

for target in builddata.buildtargets:
    for flavor in builddata.buildflavors:
        for key, value in sconfiles.items():
            env = builddata.GetEnv( target, flavor, value )

            builddir = 'build_' + projectname + '/' + env[ 'buildname' ]

            if ( 'MSVC_VERSION' in env ):
                print( "Building %s (MSVC_VER: %s)..." % ( builddir, env['MSVC_VERSION'] ) )
            else:
                print( "Building %s..." % ( builddir ) )
            SConscript( key, variant_dir = builddir, duplicate = 0, exports = 'env' )
