#!/usr/bin/env python3

################################################################################
# Copyright (c) 2021 - 2023 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
################################################################################

#
# Format sqlite trace data as json for chrome:tracing
#

import sqlite3
import argparse
import pathlib
from rocpd.tracing import generate_rpd_json

parser = argparse.ArgumentParser(description='convert RPD to json for chrome tracing')
parser.add_argument('input_rpd', type=str, help="input rpd db")
parser.add_argument('output_json', type=str, nargs='?', help="chrome tracing json output")
parser.add_argument('--start', type=str, help="start time - default us or percentage %%. Number only is interpreted as us. Number with %% is interpreted as percentage")
parser.add_argument('--end', type=str, help="end time - default us or percentage %%. See help for --start")
parser.add_argument('--format', type=str, default="object", help="chrome trace format, array or object")
args = parser.parse_args()

if args.output_json is None:
    args.output_json = str(pathlib.PurePath(args.input_rpd).with_suffix(".json"))

connection = sqlite3.connect(args.input_rpd)

with open(args.output_json, 'w', encoding="utf-8") as outfile:
    generate_rpd_json(connection, outfile,
                      start=args.start, end=args.end,
                      trace_format=args.format,
                      verbose=True)

connection.close()
