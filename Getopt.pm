# Verilog::Getopt.pm -- Verilog command line parsing
######################################################################
#
# Copyright 2000-2008 by Wilson Snyder.  This program is free software;
# you can redistribute it and/or modify it under the terms of either the GNU
# General Public License or the Perl Artistic License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
######################################################################

package Verilog::Getopt;
require 5.000;
require Exporter;

use strict;
use vars qw($VERSION $Debug %Skip_Basenames);
use Carp;
use IO::File;
use File::Basename;
use File::Spec;
use Cwd;

######################################################################
#### Configuration Section

$VERSION = '3.044';

# Basenames we should ignore when recursing directories,
# Because they contain large files of no relevance
foreach ( '.', '..',
	  'CVS',
	  '.svn',
	  '.snapshot',
	  'blib',
	  ) {
    $Skip_Basenames{$_} = 1;
}

#######################################################################
#######################################################################
#######################################################################

sub new {
    @_ >= 1 or croak 'usage: Verilog::Getopt->new ({options})';
    my $class = shift;		# Class (Getopt Element)
    $class ||= "Verilog::Getopt";

    my $self = {defines => {},
		incdir => ['.', ],
		module_dir => ['.', ],
		libext => ['.v', ],
		library => [ ],
		gcc_style => 1,
		vcs_style => 1,
		fileline => 'Command_Line',
		unparsed => [],
		define_warnings => 1,
		@_
		};
    bless $self, $class;
    return $self;
}

#######################################################################
# Option parsing

sub parameter_file {
    my $self = shift;
    my $filename = shift;

    print "*parameter_file $filename\n" if $Debug;
    my $fh = IO::File->new("<$filename") or die "%Error: ".$self->fileline().": $! $filename\n";
    my $hold_fileline = $self->fileline();
    while (my $line = $fh->getline()) {
	chomp $line;
	$line =~ s/\/\/.*$//;
	next if $line =~ /^\s*$/;
	$self->fileline ("$filename:$.");
	my @p = (split /\s+/,"$line ");
	$self->_parameter_parse(@p);
    }
    $fh->close();
    $self->fileline($hold_fileline);
}

sub parameter {
    my $self = shift;
    # Parse VCS like parameters, and perform standard setup based on it
    # Return list of leftover parameters
    @{$self->{unparsed}} = ();
    $self->_parameter_parse(@_);
    return @{$self->{unparsed}};
}

sub _parameter_parse {
    my $self = shift;
    # Internal: Parse list of VCS like parameters, and perform standard setup based on it
    foreach my $param (@_) {
	next if ($param =~ /^\s*$/);
	print " parameter($param)\n" if $Debug;

	### GCC & VCS style
	if ($param eq '-f') {
	    $self->{_parameter_next} = $param;
	}

	### VCS style
	elsif (($param eq '-v'
		|| $param eq '-y') && $self->{vcs_style}) {
	    $self->{_parameter_next} = $param;
	}
	elsif ($param =~ /^\+libext\+(.*)$/ && $self->{vcs_style}) {
	    my $ext = $1;
	    foreach (split /\+/, $ext) {
		$self->libext($_);
	    }
	}
	elsif ($param =~ /^\+incdir\+(.*)$/ && $self->{vcs_style}) {
	    $self->incdir($1);
	}
	elsif (($param =~ /^\+define\+([^+=]*)[+=](.*)$/
		|| $param =~ /^\+define\+(.*?)()$/) && $self->{vcs_style}) {
	    $self->define ($1, $2);
	}
	# Ignored
	elsif ($param =~ /^\+librescan$/ && $self->{vcs_style}) {
	}

	### GCC style
	elsif (($param =~ /^-D([^=]*)=(.*)$/
		|| $param =~ /^-D([^=]*)()$/) && $self->{gcc_style}) {
	    $self->define($1,$2);
	}
	elsif (($param =~ /^-U([^=]*)$/) && $self->{gcc_style}) {
	    $self->undef($1);
	}
	elsif ($param =~ /^-I(.*)$/ && $self->{gcc_style}) {
	    $self->incdir($1);
	}

	# Second parameters
	elsif ($self->{_parameter_next}) {
	    my $pn = $self->{_parameter_next};
	    $self->{_parameter_next} = undef;
	    if ($pn eq '-f') {
		$self->parameter_file ($self->file_substitute($param));
	    }
	    elsif ($pn eq '-v') {
		$self->library ($param);
	    }
	    elsif ($pn eq '-y') {
		$self->module_dir ($param);
	    }
	    else {
		die "%Error: ".$self->fileline().": Bad internal next param ".$pn;
	    }
	}

	else { # Unknown
	    push @{$self->{unparsed}}, $param;
	}
    }
}

#######################################################################
# Accessors

sub fileline {
    my $self = shift;
    if (@_) { $self->{fileline} = shift; }
    return ($self->{fileline});
}
sub incdir {
    my $self = shift;
    if (@_) {
	my $token = shift;
	print "incdir $token\n" if $Debug;
	if (ref $token) {
	    @{$self->{incdir}} = @{$token};
	} else {
	    push @{$self->{incdir}}, $self->file_abs($token);
	}
	$self->file_path_cache_flush();
    }
    return (wantarray ? @{$self->{incdir}} : $self->{incdir});
}
sub libext {
    my $self = shift;
    if (@_) {
	my $token = shift;
	print "libext $token\n" if $Debug;
	if (ref $token) {
	    @{$self->{libext}} = @{$token};
	} else {
	    push @{$self->{libext}}, $token;
	}
	$self->file_path_cache_flush();
    }
    return (wantarray ? @{$self->{libext}} : $self->{libext});
}
sub library {
    my $self = shift;
    if (@_) {
	my $token = shift;
	print "library $token\n" if $Debug;
	if (ref $token) {
	    @{$self->{library}} = @{$token};
	} else {
	    push @{$self->{library}}, $self->file_abs($token);
	}
    }
    return (wantarray ? @{$self->{library}} : $self->{library});
}
sub module_dir {
    my $self = shift;
    if (@_) {
	my $token = shift;
	print "module_dir $token\n" if $Debug;
	if (ref $token) {
	    @{$self->{module_dir}} = @{$token};
	} else {
	    push @{$self->{module_dir}}, $self->file_abs($token);
	}
	$self->file_path_cache_flush();
    }
    return (wantarray ? @{$self->{module_dir}} : $self->{module_dir});
}
sub depend_files {
    my $self = shift;
    if (@_) {
	if (ref $_[0]) {
	    $self->{depend_files} = {};
	    foreach my $fn (@{$_[0]}) {
		$self->{depend_files}{$fn} = 1;
	    }
	} else {
	    foreach my $fn (@_) {
		print "depend_files $fn\n" if $Debug;
		$self->{depend_files}{$fn} = 1;
	    }
	}
    }
    my @list = (sort (keys %{$self->{depend_files}}));
    return (wantarray ? @list : \@list);
}

sub get_parameters {
    my $self = shift;
    my %args = (gcc_stlyle => $self->{gcc_style},);
    # Defines
    my @params = ();
    foreach my $def (sort (keys %{$self->{defines}})) {
	my $defvalue = $self->defvalue($def);
	$defvalue = "=".($defvalue||"") if (defined $defvalue && $defvalue ne "");
	if ($args{gcc_style}) {
	    push @params, "-D${def}${defvalue}";
	} else {
	    push @params, "+define+${def}${defvalue}";
	}
    }
    # Put all libexts on one line, else NC-Verilog will bitch
    my $exts="";
    foreach my $ext ($self->libext()) {
	$exts = "+libext" if !$exts;
	$exts .= "+$ext";
    }
    push @params, $exts if $exts;
    # Includes...
    foreach my $dir ($self->incdir()) {
	if ($args{gcc_style}) {
	    push @params, "-I${dir}";
	} else {
	    push @params, "+incdir+${dir}";
	}
    }
    foreach my $dir ($self->module_dir()) {
	push @params, "-y", $dir;
    }
    foreach my $dir ($self->library()) {
	push @params, "-v", $dir;
    }
    return (@params);
}

sub write_parameters_file {
    my $self = shift;
    my $filename = shift;
    # Write get_parameters to a file
    my $fh = IO::File->new(">$filename") or croak "%Error: $! writing $filename,";
    my @opts = $self->get_parameters();
    print $fh join("\n",@opts);
    $fh->close;
}

#######################################################################
# Utility functions

sub remove_duplicates {
    my $self = ref $_[0] && shift;
    # return list in same order, with any duplicates removed
    my @rtn;
    my %hit;
    foreach (@_) { push @rtn, $_ unless $hit{$_}++; }
    return @rtn;
}

sub file_skip_special {
    my $self = shift;
    my $filename = shift;
    $filename =~ s!.*[/\\]!!;
    return $Skip_Basenames{$filename};
}

sub file_abs {
    my $self = shift;
    my $filename = shift;
    # return absolute filename
    # If the user doesn't want this absolutification, they can just
    # make their own derived class and override this function.
    #
    # We don't absolutify files that don't have any path,
    # as file_path() will probably be used to resolve them.
    return $filename;
    return $filename if ("" eq dirname($filename));
    return $filename if File::Spec->file_name_is_absolute($filename);
    # Cwd::abspath() requires files to exist.  Too annoying...
    $filename = File::Spec->canonpath(File::Spec->catdir(Cwd::getcwd(),$filename));
    return $filename;
}

sub file_substitute {
    my $self = shift;
    my $filename = shift;
    my $out = $filename;
    while ($filename =~ /\$([A-Za-z_0-9]+)\b/g) {
	my $var = $1;
	if (defined $ENV{$var}) {
	    $out =~ s/\$$var\b/$ENV{$var}/g;
	}
    }
    return $out;
}

sub file_path_cache_flush {
    my $self = shift;
    # Clear out a file_path cache, needed if the incdir/module_dirs change
    $self->{_file_path_cache} = {};
}

sub file_path {
    my $self = shift;
    my $filename = shift;
    my $lookup_type = shift || 'all';
    # return path to given filename using library directories & files, or undef
    # locations are cached, because -r can be a very slow operation

    defined $filename or carp "%Error: Undefined filename,";
    return $self->{_file_path_cache}{$filename} if defined $self->{_file_path_cache}{$filename};
    if (-r $filename && !-d $filename) {
	$self->{_file_path_cache}{$filename} = $filename;
	$self->depend_files($filename);
	return $filename;
    }
    # What paths to use?
    my @dirlist;
    if ($lookup_type eq 'module') {
	@dirlist = $self->module_dir();
    } elsif ($lookup_type eq 'include') {
	@dirlist = $self->incdir();
    } else {  # all
	@dirlist = ($self->incdir(), $self->module_dir());
    }
    # Expand any envvars in incdir/moduledir
    @dirlist = map {$self->file_substitute($_)} @dirlist;

    # Check each search path
    # We use both the incdir and moduledir.  This isn't strictly correct,
    # but it's fairly silly to have to specify both all of the time.
    my %checked_dir = ();
    my %checked_file = ();
    foreach my $dir (@dirlist) {
	next if $checked_dir{$dir}; $checked_dir{$dir}=1;  # -r can be quite slow
	# Check each postfix added to the file
	foreach my $postfix ("", @{$self->{libext}}) {
	    my $found = "$dir/$filename$postfix";
	    next if $checked_file{$found}; $checked_file{$found}=1;  # -r can be quite slow
	    if (-r $found && !-d $found) {
		$self->{_file_path_cache}{$filename} = $found;
		$self->depend_files($found);
		return $found;
	    }
	}
    }

    return $filename;	# Let whoever needs it discover it doesn't exist
}

sub libext_matches {
    my $self = shift;
    my $filename = shift;
    return undef if !$filename;
    foreach my $postfix (@{$self->{libext}}) {
	my $re = quotemeta($postfix) . "\$";
	return $filename if ($filename =~ /$re/);
    }
    return undef;
}

sub map_directories {
    my $self = shift;
    my $func = shift;
    # Execute map function on all directories listed in self.
    {
	my @newdir = $self->incdir();
	@newdir = map {&{$func}} @newdir;
	$self->incdir(\@newdir);
    }
    {
	my @newdir = $self->module_dir();
	@newdir = map {&{$func}} @newdir;
	$self->module_dir(\@newdir);
    }
}

#######################################################################
# Getopt functions

sub defparams {
    my $self = shift;
    my $token = shift;
    my $val = $self->{defines}{$token};
    if (!defined $val) {
	return undef;
    } elsif (ref $val) {
	return $val->[1];  # Has parameters, return param list
    } else {
	return 0;
    }
}
sub defvalue {
    my $self = shift;
    my $token = shift;
    my $val = $self->{defines}{$token};
    (defined $val) or carp "%Warning: ".$self->fileline().": No definition for $token,";
    if (ref $val) {
	return $val->[0];  # Has parameters, return just value
    } else {
	return $val;
    }
}
sub defvalue_nowarn {
    my $self = shift;
    my $token = shift;
    my $val = $self->{defines}{$token};
    if (ref $val) {
	return $val->[0];  # Has parameters, return just value
    } else {
	return $val;
    }
}
sub define {
    my $self = shift;
    if (@_) {
	my $token = shift;
	my $value = shift;
	my $params = shift||"";
	print "Define $token $params= $value\n" if $Debug;
	my $oldval = $self->{defines}{$token};
	my $oldparams;
	if (ref $oldval eq 'ARRAY') {
	    ($oldval, $oldparams) = @{$oldval};
	}
	if (defined $oldval
	    && (($oldval ne $value)
		|| (defined $oldparams && $oldparams ne $params))
	    && $self->{define_warnings}) {
	    warn "%Warning: ".$self->fileline().": Redefining `$token\n";
	}
	if ($params) {
	    $self->{defines}{$token} = [$value, $params];
	} else {
	    $self->{defines}{$token} = $value;
	}
    }
}
sub undef {
    my $self = shift;
    my $token = shift;
    my $oldval = $self->{defines}{$token};
    # We no longer warn about undefing something that doesn't exist, as other compilers don't
    #(defined $oldval or !$self->{define_warnings})
    #	or carp "%Warning: ".$self->fileline().": No definition to undef for $token,";
    delete $self->{defines}{$token};
}

sub remove_defines {
    my $self = shift;
    my $sym = shift;
    my $val = "x";
    while (defined $val) {
	last if $sym eq $val;
	(my $xsym = $sym) =~ s/^\`//;
	$val = $self->defvalue_nowarn($xsym);  #Undef if not found
	$sym = $val if defined $val;
    }
    return $sym;
}

######################################################################
### Package return
1;
__END__

=pod

=head1 NAME

Verilog::Getopt - Get Verilog command line options

=head1 SYNOPSIS

  use Verilog::Getopt;

  my $opt = new Verilog::Getopt;
  $opt->parameter (qw( +incdir+standard_include_directory ));

  @ARGV = $opt->parameter (@ARGV);
  ...
  print "Path to foo.v is ", $opt->file_path('foo.v');

=head1 DESCRIPTION

Verilog::Getopt provides standardized handling of options similar to
Verilog/VCS and cc/GCC.

=over 4

=item $opt = Verilog::Getopt->new ( I<opts> )

Create a new Getopt.  If gcc_style=>0 is passed as a parameter, parsing of
GCC-like parameters is disabled.  If vcs_style=>0 is passed as a parameter,
parsing of VCS-like parameters is disabled.

=item $self->file_path ( I<filename>, [I<lookup-type>] )

Returns a new path to the filename, using the library directories and
search paths to resolve the file.  Optional lookup-type is
'module','include', or 'all', to use only module_dirs, incdirs, or both for
the lookup.

=item $self->get_parameters ( )

Returns a list of parameters that when passed through $self->parameter()
should result in the same state.  Often this is used to form command lines
for downstream programs that also use Verilog::Getopt.

=item $self->parameter ( \@params )

Parses any recognized parameters in the referenced array, removing the
standard parameters and returning a array with all unparsed parameters.

The below list shows the VCS-like parameters that are supported, and the
functions that are called:

    +libext+I<ext>+I<ext>...	libext (I<ext>)
    +incdir+I<dir>		incdir (I<dir>)
    +define+I<var>[+=]I<value>	define (I<var>,I<value>)
    +define+I<var>		define (I<var>,undef)
    +librescan		Ignored
    -f I<file>		Parse parameters in file
    -v I<file>		library (I<file>)
    -y I<dir>		module_dir (I<dir>)
    all others		Put in returned list

The below list shows the GCC-like parameters that are supported, and the
functions that are called:

    -DI<var>=I<value>		define (I<var>,I<value>)
    -DI<var>		define (I<var>,undef)
    -UI<var>		undefine (I<var>)
    -II<dir>		incdir (I<dir>)
    -f I<file>		Parse parameters in file
    all others		Put in returned list

=item $self->write_parameters_file ( I<filename> )

Write the output from get_parameters to the specified file.

=back

=head1 ACCESSORS

=over 4

=item $self->define ( $token, $value )

This method is called when a define is recognized.  The default behavior
loads a hash that is used to fulfill define references.  This function may
also be called outside parsing to predefine values.

=item $self->defparams ( $token )

This method returns the parameter list of the define.  This will be defined,
but false, if the define does not have arguments.

=item $self->defvalue ( $token )

This method returns the value of a given define, or prints a warning.

=item $self->defvalue_nowarn ( $token )

This method returns the value of a given define, or undef.

=item $self->depend_files ()

Returns reference to list of filenames referenced with file_path, useful
for creating dependency lists.  With argument, adds that file.  With list
reference argument, sets the list to the argument.

=item $self->file_abs ( $filename )

Using the incdir and libext lists, convert the specified module or filename
("foo") to a absolute filename ("include/dir/foo.v").

=item $self->file_skip_special ( $filename )

Return true if the filename is one that generally should be ignored when
recursing directories, such as for example, ".", "CVS", and ".svn".

=item $self->file_substitute ( $filename )

Removes existing environment variables from the provided filename.  Any
undefined variables are not substituted nor cause errors.

=item $self->incdir ()

Returns reference to list of include directories.  With argument, adds that
directory.

=item $self->libext ()

Returns reference to list of library extensions.  With argument, adds that
extension.

=item $self->libext_matches (I<filename>)

Returns true if the passed filename matches the libext.

=item $self->library ()

Returns reference to list of libraries.  With argument, adds that library.

=item $self->module_dir ()

Returns reference to list of module directories.  With argument, adds that
directory.

=item $self->remove_defines ( $token )

Return string with any definitions in the token removed.

=item $self->undef ( $token )

Deletes a hash element that is used to fulfill define references.  This
function may also be called outside parsing to erase a predefined value.

=back

=head1 DISTRIBUTION

Verilog-Perl is part of the L<http://www.veripool.org/> free Verilog EDA
software tool suite.  The latest version is available from CPAN and from
L<http://www.veripool.org/verilog-perl>.

Copyright 2000-2008 by Wilson Snyder.  This package is free software; you
can redistribute it and/or modify it under the terms of either the GNU
Lesser General Public License or the Perl Artistic License.

=head1 AUTHORS

Wilson Snyder <wsnyder@wsnyder.org>

=head1 SEE ALSO

L<Verilog-Perl>,
L<Verilog::Language>

=cut
