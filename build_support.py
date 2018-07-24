#!python

import os
import sys
import platform
from SCons.Script import *

# export SCONSFLAGS="-Q"

class BuildData:
    def __init__(self):
        # Default to -j XX
        if GetOption( 'num_jobs' ) == 1:
            num_cpus = 8
            try:
                import multiprocessing
                num_cpus = multiprocessing.cpu_count()
            except (ImportError, NotImplementedError):
                pass
            SetOption( 'num_jobs', num_cpus )

        # self.os_environ = os.environ
        self.platform_arch = platform.architecture()
        self.platform_machine = platform.machine()
        self.host_system = platform.system()

        self.envscache = {}

        self.vars = Variables()
        self.vars.Add( BoolVariable('verbose', 'Show command lines', 0) )
        self.vars.Add( BoolVariable('debug', 'Build debug version', 0) )
        self.vars.Add( BoolVariable('release', 'Build release version', 0) )
        if self.host_system == 'Linux':
            self.vars.Add( BoolVariable('asan', 'Build with address sanitizer', 0) )
            self.vars.Add( BoolVariable('gprof', 'Build with google gperftools libprofiler', 0) )

        # TODO: Add targets option: targets=Linux-i386,Win64,Win32,...
        # TODO: Add spew about which option (asan, gprof, etc) is on

        self.buildflavors = []
        self.buildtargets = []

        env = Environment(variables = self.vars)
        if env['debug']:
            self.buildflavors.append('debug')
        if env['release']:
            self.buildflavors.append('release')

        if not self.buildflavors:
            self.buildflavors.append('release')

        if not self.buildtargets:
            if self.host_system == 'Linux':
                # Linux-x86_64 or Linux-i386
                self.buildtargets = [ 'Linux-' + self.platform_machine ]
            else:
                self.buildtargets = [ self.host_system ]


    def BuildEnvLinux(self, buildname, target, flavor):
        env = Environment( platform = Platform( 'posix' ) )

        self.vars.Update( env )

        Help( self.vars.GenerateHelpText( env ) )

        unknown = self.vars.UnknownVariables()
        if unknown:
            print( self.vars.GenerateHelpText(env) )
            print( "ERROR: Unknown variables:", unknown.keys() )
            Exit( 1 )

        env['buildname'] = buildname
        env['buildtarget'] = target
        env['buildflavor'] = flavor

        if target == 'Linux-i386':
            env.Append( CCFLAGS = '-m32' )

        warnings = [
            '-Wall',
            '-Wextra',
            '-Wpedantic',
            '-Wmissing-include-dirs',
            '-Wformat=2',
            '-Wshadow',
            '-Wno-unused-parameter',
            '-Wno-missing-field-initializers' ]
        env.Append( CCFLAGS = warnings )

        env.Append( CPPDEFINES = [ '-D_LARGEFILE64_SOURCE=1', '-D_FILE_OFFSET_BITS=64' ] )

        if env['gprof']:
            env.Append( CCFLAGS = [ '-DGPROFILER' ] )
            env.ParseConfig( 'pkg-config --cflags --libs libprofiler' )
        elif env['asan']:
            ASAN_FLAGS = [
                '-fno-omit-frame-pointer',
                '-fno-optimize-sibling-calls',
                '-fsanitize=address', # fast memory error detector (heap, stack, global buffer overflow, and use-after free)
                '-fsanitize=leak', # detect leaks
                '-fsanitize=undefined', # fast undefined behavior detector
                '-fsanitize=float-divide-by-zero', # detect floating-point division by zero;
                '-fsanitize=bounds', # enable instrumentation of array bounds and detect out-of-bounds accesses;
                '-fsanitize=object-size', # enable object size checkin
                ]
            env.Append( CCFLAGS = ASAN_FLAGS )
            env.Append( LINKFLAGS = ASAN_FLAGS )

        env.Append( LINKFLAGS = [ '-Wl,--no-as-needed' ] )
        env.Append( LINKFLAGS = [ '-march=native', '-gdwarf-4', '-g2', '-Wl,--build-id=sha1' ] )

        if flavor == 'debug':
            env.MergeFlags( '-O0 -DDEBUG' )
            env.MergeFlags( '-D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC -D_GLIBCXX_SANITIZE_VECTOR' )
        else:
            env.MergeFlags( '-O2 -DNDEBUG' )

        return env

    def GetEnv(self, target, flavor):
        # Check cache for this environment
        buildname = target + '-' + flavor
        if buildname in self.envscache:
            return self.envscache[ buildname ].Clone()

        if target == 'Linux-x86_64' or target == 'Linux-i386':
            env = self.BuildEnvLinux( buildname, target, flavor )
        else:
            print( "ERROR: target %s not defined..." % target )
            Exit( 2 )

        if not env['verbose']:
            env['CCCOMSTR'] = " Compiling ${SOURCE}..."
            env['CXXCOMSTR'] = " Compiling ${SOURCE}..."
            env['LINKCOMSTR'] = "Linking $TARGET"

        self.envscache[ buildname  ] = env
        return env.Clone()
