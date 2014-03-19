use lib ('.', "../../Tools/perl_lib","perl_lib");
use Cwd;
use File::Path;
use Getopt::Long;
use Tools qw(InstallNameTool);

my $root = getcwd();
my $skipbuild=0;
my $debug = 0;
my $minimal = 0;
my $iphone_simulator = 0;
my $jobs = 4;
my $xcodePath = '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform';

GetOptions(
   "skipbuild=i"=>\$skipbuild,
   "debug=i"=>\$debug,
   "minimal=i"=>\$minimal,
   "iphone_simulator=i"=>\$iphone_simulator
) or die ("illegal cmdline options");

my $teamcity=0;
if ($ENV{UNITY_THISISABUILDMACHINE})
{
	print "rmtree-ing $root/builds because we're on a buildserver, and want to make sure we don't include old artifacts\n";
	rmtree("$root/builds");
	$teamcity=1;
} else {
	print "not rmtree-ing $root/builds, as we're not on a buildmachine";
	if (($debug==0) && ($skipbuild==0))
	{
		print "\n\nARE YOU SURE YOU DONT WANT TO MAKE A DEBUG BUILD?!?!?!!!!!\n\n\n";
	}
}
$ENV{'LIBTOOLIZE'} = 'glibtoolize';

my @arches = ('x86_64','i386');
if ($iphone_simulator || $minimal) {
	@arches = ('i386');
}

for my $arch (@arches)
{
	print "Building for architecture: $arch\n";

	my $macversion = '10.5';
	my $sdkversion = '10.6';
	if ($arch eq 'x86_64') {
		$macversion = '10.6';
	}

	# Make architecture-specific targets and lipo at the end
	my $bintarget = "$root/builds/monodistribution/bin-$arch";
	my $libtarget = "$root/builds/embedruntimes/osx-$arch";

	if ($minimal)
	{
		$libtarget = "$root/builds/embedruntimes/osx-minimal";
	}
	print("libtarget: $libtarget\n");

	system("rm $bintarget/mono");
	system("rm $libtarget/libmono.0.dylib");
	system("rm -rf $libtarget/libmono.0.dylib.dSYM");

	if (not $skipbuild)
	{
		if ($debug)
		{
			$ENV{CFLAGS} = "-arch $arch -g -O0 -D_XOPEN_SOURCE=1 -DMONO_DISABLE_SHM=1 -DDISABLE_SHARED_HANDLES=1";
			$ENV{CXXFLAGS} = $ENV{CFLAGS};
			$ENV{LDFLAGS} = "-arch $arch";
		} else
		{
			# -Os (and -O2 and even -O1) on llvm break/crash the soft debugger
			# Work around this for now by manually enabling -Os optimizations from the llvm doc
			my $Os = '-fglobal-alloc-prefer-bytes -fomit-frame-pointer -fdefer-pop -fguess-branch-probability -fcprop-registers -fif-conversion -fif-conversion2 -ftree-ccp -ftree-dce -ftree-dominator-opts -ftree-dse -ftree-ter -ftree-lrs -ftree-sra -ftree-copyrename -ftree-fre -ftree-ch -funit-at-a-time -fmerge-constants -fthread-jumps -fcrossjumping -foptimize-sibling-calls -fcse-follow-jumps  -fcse-skip-blocks -fgcse  -fgcse-lm -fexpensive-optimizations -frerun-cse-after-loop -fcaller-saves -fpeephole2 -fschedule-insns  -fschedule-insns2 -fsched-interblock  -fsched-spec -fregmove -fstrict-aliasing -fstrict-overflow -fdelete-null-pointer-checks -freorder-functions -ftree-vrp -ftree-pre';
			$ENV{CFLAGS} = "-arch $arch -O0 $Os -D_XOPEN_SOURCE=1 -DMONO_DISABLE_SHM=1 -DDISABLE_SHARED_HANDLES=1";  #optimize for size
			$ENV{CXXFLAGS} = $ENV{CFLAGS};
			$ENV{LDFLAGS} = "-arch $arch";
		}

		if ($iphone_simulator)
		{
			$ENV{CFLAGS} = "-D_XOPEN_SOURCE=1 -DTARGET_IPHONE_SIMULATOR -g -O0";
			$macversion = "10.6";
			$sdkversion = "10.6";
		} else {
			$ENV{'MACSDKOPTIONS'} = "-mmacosx-version-min=$macversion -isysroot $xcodePath/Developer/SDKs/MacOSX$sdkversion.sdk";
		}
		
		#this will fail on a fresh working copy, so don't die on it.
		system("make distclean");
		#were going to tell autogen to use a specific cache file, that we purposely remove before starting.
		#that way, autogen is forced to do all its config stuff again, which should make this buildscript
		#more robust if other targetplatforms have been built from this same workincopy
		system("rm osx.cache");

		chdir("$root/eglib") eq 1 or die ("Failed chdir 1");
		
		#this will fail on a fresh working copy, so don't die on it.
		system("make distclean");
		system("autoreconf -i") eq 0 or die ("Failed autoreconfing eglib");
		chdir("$root") eq 1 or die ("failed to chdir 2");
		system("autoreconf -i") eq 0 or die ("Failed autoreconfing mono");
		my @autogenparams = ();
		unshift(@autogenparams, "--cache-file=osx.cache");
		unshift(@autogenparams, "--disable-mcs-build");
		unshift(@autogenparams, "--with-glib=embedded");
		unshift(@autogenparams, "--disable-nls");  #this removes the dependency on gettext package

		# From Massi: I was getting failures in install_name_tool about space
		# for the commands being too small, and adding here things like
		# $ENV{LDFLAGS} = '-headerpad_max_install_names' and
		# $ENV{LDFLAGS} = '-headerpad=0x40000' did not help at all (and also
		# adding them to our final gcc invocation to make the bundle).
		# Lucas noticed that I was lacking a Mono prefix, and having a long
		# one would give us space, so here is this silly looong prefix.
		unshift(@autogenparams, "--prefix=/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890/1234567890");

		if ($minimal)
		{
			unshift(@autogenparams,"--enable-minimal=aot,logging,com,profiler,debug");
		}

		print("\n\n\n\nCalling configure with these parameters: ");
		system("echo", @autogenparams);
		print("\n\n\n\n\n");
		system("calling ./configure",@autogenparams);
		system("./configure", @autogenparams) eq 0 or die ("failing configuring mono");

		system("make clean") eq 0 or die ("failed make cleaning");
		if ($iphone_simulator)
		{
			system("perl -pi -e 's/#define HAVE_STRNDUP 1//' eglib/config.h");
		}
		system("make") eq 0 or die ("failing runnig make for mono");
	}

	chdir($root);

	mkpath($bintarget);
	mkpath($libtarget);

	if (!$iphone_simulator)
	{
		my $cmdline = "gcc -arch $arch -bundle -reexport_library mono/mini/.libs/libmono.a -isysroot $xcodePath/Developer/SDKs/MacOSX$sdkversion.sdk -mmacosx-version-min=$macversion -all_load -liconv -o $libtarget/MonoBundleBinary";
		print "About to call this cmdline to make a bundle:\n$cmdline\n";
		system($cmdline) eq 0 or die("failed to link libmono.a into mono bundle");

		print "Symlinking libmono.dylib\n";
		system("ln","-f", "$root/mono/mini/.libs/libmono.0.dylib","$libtarget/libmono.0.dylib") eq 0 or die ("failed symlinking libmono.0.dylib");

		print "Symlinking libmono.a\n";
		system("ln", "-f", "$root/mono/mini/.libs/libmono.a","$libtarget/libmono.a") eq 0 or die ("failed symlinking libmono.a");

		if (not $ENV{"UNITY_THISISABUILDMACHINE"})
		{
			rmtree ("$libtarget/libmono.0.dylib.dSYM");
			system ('cp', '-R', "$root/mono/mini/.libs/libmono.0.dylib.dSYM","$libtarget/libmono.0.dylib.dSYM") eq 0 or die ("Failed copying libmono.0.dylib.dSYM");
		}
	 
		if ($ENV{"UNITY_THISISABUILDMACHINE"})
		{
		#	system("strip $libtarget/libmono.0.dylib") eq 0 or die("failed to strip libmono");
		#	system("strip $libtarget/MonoBundleBinary") eq 0 or die ("failed to strip MonoBundleBinary");
			system("echo \"mono-runtime-osx = $ENV{'BUILD_VCS_NUMBER'}\" > $root/builds/versions.txt");
		}

		InstallNameTool("$libtarget/libmono.0.dylib", "\@executable_path/../Frameworks/MonoEmbedRuntime/osx/libmono.0.dylib");

		system("ln","-f","$root/mono/mini/mono","$bintarget/mono") eq 0 or die("failed symlinking mono executable");
		system("ln","-f","$root/mono/metadata/pedump","$bintarget/pedump") eq 0 or die("failed symlinking pedump executable");
	}
}


if (!$iphone_simulator)
{
	# Create universal binaries
	mkpath ("$root/builds/embedruntimes/osx");
	for $file ('libmono.0.dylib','libmono.a') {
		system ('lipo', "$root/builds/embedruntimes/osx-i386/$file", "$root/builds/embedruntimes/osx-x86_64/$file", '-create', '-output', "$root/builds/embedruntimes/osx/$file");
	}
	system('cp', "$root/builds/embedruntimes/osx-i386/MonoBundleBinary", "$root/builds/embedruntimes/osx/MonoBundleBinary");

	mkpath ("$root/builds/monodistribution/bin");
	for $file ('mono','pedump') {
		system ('lipo', "$root/builds/monodistribution/bin-i386/$file", '-create', '-output', "$root/builds/monodistribution/bin/$file");
		# Don't add 64bit executables for now...
		# system ('lipo', "$root/builds/monodistribution/bin-i386/$file", "$root/builds/monodistribution/bin-x86_64/$file", '-create', '-output', "$root/builds/monodistribution/bin/$file");
	}
	
	if ($ENV{"UNITY_THISISABUILDMACHINE"}) {
		# Clean up temporary arch-specific directories
		rmtree("$root/builds/embedruntimes/osx-i386");
		rmtree("$root/builds/embedruntimes/osx-x86_64");
		rmtree("$root/builds/monodistribution/bin-i386");
		rmtree("$root/builds/monodistribution/bin-x86_64");
	}
}
