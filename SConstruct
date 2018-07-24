#!python

from build_support import *

projectname = 'gpuvis'
sconfiles = [ 'src/gpuvis.scons' ]

builddata = BuildData()
Export('builddata')

builddata.vars.Add( BoolVariable( 'gtk3', 'Build with gtk3 open file dialog', 1 ) )
builddata.vars.Add( BoolVariable( 'freetype', 'Build with freetype', 1 ) )

for target in builddata.buildtargets:
    for flavor in builddata.buildflavors:
        env = builddata.GetEnv( target, flavor )

        builddir = 'build_' + projectname + '/' + env[ 'buildname' ]

        print( "Building %s..." % ( builddir ) )
        SConscript( sconfiles, variant_dir = builddir, duplicate = 0, exports = 'env' )
