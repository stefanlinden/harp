"""Copyright (C) 2015-2016 S[&]T, The Netherlands.

This file is part of HARP.

HARP is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2 of the License, or (at your option) any later version.

HARP is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
HARP; if not, write to the Free Software Foundation, Inc., 59 Temple Place,
Suite 330, Boston, MA  02111-1307  USA

"""

from __future__ import print_function

import cffi
import re
import sys

# Import the StringIO class that uses the default str type on each major version
# of Python.
#
# The StringIO.StringIO class is Python 2 specific and works with byte strings,
# i.e. instances of class str.
#
# The io.StringIO class exists in both Python 2 and Python 3, and always uses
# unicode strings, i.e. instances of class unicode on Python 2 and of class str
# on Python 3.
#
try:
    from cStringIO import StringIO
except ImportError:
    try:
        from StringIO import StringIO
    except ImportError:
        from io import StringIO

class Error(Exception):
    pass

class SyntaxError(Error):
    pass

def read_header_file(filename):
    cdefs = StringIO()
    active = False
    with open(filename) as header_file:
        for (line_no, line) in enumerate(header_file):
            if re.match(r"^\s*/\*\s*\*CFFI-ON\*\s*\*/\s*$", line) or re.match(r"^\s*//\s*\*CFFI-ON\*\s*$", line):
                if active:
                    raise SyntaxError("%s:%lu: CFFI-ON marker inside CFFI block" % (filename, line_no))
                active = True
                continue

            if re.match(r"^\s*/\*\s*\*CFFI-OFF\*\s*\*/\s*$", line) or re.match(r"^\s*//\s*\*CFFI-OFF\*\s*$", line):
                if not active:
                    raise SyntaxError("%s:%lu: CFFI-OFF marker outside CFFI block" % (filename, line_no))
                active = False
                continue

            # Remove LIBHARP_API prefix.
            line = re.sub("^\s*LIBHARP_API\s*", "", line)

            # Remove brackets from #define statements, since in ABI mode cffi only accepts #define followed by a numeric
            # constant.
            match_obj = re.match(r"^\s*#define\s+(\w+)\s+\(([^)]+)\)\s*$", line)
            if match_obj:
                name = match_obj.group(1)

                try:
                    value = int(match_obj.group(2))
                except ValueError:
                    try:
                        value = float(match_obj.group(2))
                    except ValueError:
                        # If the value cannot be parsed as an int or float, output the original line without
                        # modification.
                        pass
                    else:
                        line = "#define %s %f\n" % (name, value)
                else:
                    line = "#define %s %d\n" % (name, value)

            if active:
                cdefs.write(line)

    if active:
        raise SyntaxError("%s:%lu: unterminated CFFI block; CFFI-OFF marker missing" % (filename, line_no))

    return cdefs.getvalue()

def main(header_path, output_path):
    ffi = cffi.FFI()
    ffi.set_source("_harpc", None)
    ffi.cdef(read_header_file(header_path))
    ffi.emit_python_code(output_path)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: %s <path to harp.h.in> <output Python file>" % sys.argv[0])
        print("generate Python wrapper for the HARP C library using cffi (ABI level, out-of-line)")
        sys.exit(1)

    try:
        main(sys.argv[1], sys.argv[2])
    except Error as _error:
        print("ERROR: %s" % _error)
        sys.exit(1)