#!python

from build_support import *

projectname = 'gpuvis'
sconfiles = {}
sconfiles[ 'src/gpuvis.scons' ] = { 'MSVC_VERSION' : '10.0' }

builddata = BuildData()
Export('builddata')

builddata.vars.Add( BoolVariable( 'gtk3', 'Build with gtk3 open file dialog', 1 ) )
builddata.vars.Add( BoolVariable( 'freetype', 'Build with freetype', 1 ) )

for target in builddata.buildtargets:
    for flavor in builddata.buildflavors:
        for key, value in sconfiles.items():
            env = builddata.GetEnv( target, flavor, value )

            builddir = 'build_' + projectname + '/' + env[ 'buildname' ]

            print( "Building %s..." % ( builddir ) )
            SConscript( key, variant_dir = builddir, duplicate = 0, exports = 'env' )
