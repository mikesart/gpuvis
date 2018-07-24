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
                self.buildtargets = [ 'Lnx64' ]
            elif self.host_system == 'Windows':
                self.buildtargets = [ 'Win64' ]
            else:
                self.buildtargets = [ self.host_system ]

    def BuildEnvLinux(self, buildname, target, flavor, values):
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

        if target == 'Lnx32':
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

    def BuildEnvWindows(self, buildname, target, flavor, values):
        # MSVC_VERSION values:
        # 14.0
        # 14.0Exp # Express version
        # 12.0
        # 12.0Exp
        # 11.0
        # 11.0Exp
        # 10.0
        # 10.0Exp
        # 9.0
        # 9.0Exp
        # 8.0
        # 8.0Exp
        # 7.1
        # 7.0
        # 6.0
        msvcver = None
        if 'MSVC_VERSION' in values:
            msvcver = values['MSVC_VERSION']

        env = Environment( platform = Platform( 'win32' ), MSVC_VERSION = msvcver )

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

        env.Append( CPPDEFINES = [ 'WIN32', '_WINDOWS', '_CRT_SECURE_NO_WARNINGS' ] )

        env.Append( CPPFLAGS = [ '/W3' ] )
        # /Zi: debug info goes into a PDB file
        # /FS: force to use MSPDBSRV.EXE (serializes access to .pdb files for multi-core builds)
        env.Append( CCFLAGS = [ '/Zi', '/FS' ] )
        # /DEBUG : linker to create a .pdb file which WinDbg and Visual Studio will use to resolve symbols if you want to debug a release-mode image.
        env.Append( LINKFLAGS = [ '/DEBUG' ] )

        if flavor == 'debug':
            env.Append( CCFLAGS = [ '/Od' ] )
            env.Append( CPPDEFINES = [ '/DDEBUG' ] )
        else:
            env.Append( CCFLAGS = [ '/O2', '/Ob1' ] )
            env.Append( CPPDEFINES = [ '/DNDEBUG' ] )

        return env

    def GetEnv(self, target, flavor, values):
        # Check cache for this environment
        buildname = target + '-' + flavor
        if buildname in self.envscache:
            return self.envscache[ buildname ].Clone()

        if target == 'Lnx32' or target == 'Lnx64':
            env = self.BuildEnvLinux( buildname, target, flavor, values )
        elif target == 'Win32' or target == 'Win64':
            env = self.BuildEnvWindows( buildname, target, flavor, values )
        else:
            print( "ERROR: target %s not defined..." % target )
            Exit( 2 )

        if not env['verbose']:
            env['CCCOMSTR'] = " Compiling ${SOURCE}..."
            env['CXXCOMSTR'] = " Compiling ${SOURCE}..."
            env['LINKCOMSTR'] = "Linking $TARGET"

        self.envscache[ buildname  ] = env
        return env.Clone()
