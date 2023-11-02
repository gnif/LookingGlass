#!/usr/bin/env python3

# Copyright 2023 The Looking Glass Authors
# https://looking-glass.io
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 59
# Temple Place, Suite 330, Boston, MA 02111-1307 USA

import os
import sys
import difflib
import argparse
from typing import BinaryIO
from collections.abc import Generator


class parsed_args(argparse.Namespace):
    add_words: list[str]
    quiet: bool
    save: bool
    words_files: list[str]


class sorted_words:
    new_words: list[str]
    old_words: list[str]
    diff: Generator

    def __init__(self, new_words, old_words, diff):
        self.new_words = new_words
        self.old_words = old_words
        self.diff = diff


def sort_words(old_words: list[str], words_path: str, add_words: list[str] = []) -> sorted_words:
    new_words: list[str] = add_words.copy()
    for new_word in old_words:
        if not (new_word in new_words):
            new_words.append(new_word)
    new_words.sort(key=str.lower)
    diff: Generator = difflib.unified_diff(old_words, new_words, os.path.join('current', words_path), os.path.join('sorted', words_path))
    return sorted_words(new_words, old_words, diff)


parseargs: argparse.ArgumentParser = argparse.ArgumentParser(
    prog='sort_words.py',
    description='Reads word list from input file(s) and outputs patch to sort it',
    epilog=''
)

parseargs.add_argument('-s', '--save',
                       help='save changes to file(s) instead of just outputting a patch',
                       dest='save', action=argparse.BooleanOptionalAction)

parseargs.add_argument('-q', '--quiet',
                       help='don\'t output a diff',
                       dest='quiet', action=argparse.BooleanOptionalAction)

parseargs.add_argument('-a', '--add-word', metavar='word',
                       help='add this word to the sorted file(s), can be specified multiple times',
                       dest='add_words', action='append', default=[])

parseargs.add_argument('words_files', metavar='wordlist(s)',
                       help='(Optional) filenames to sort, defaults to \'words.txt\'',
                       nargs='*', default=['words.txt'])


if __name__ == '__main__':
    args: parsed_args = parseargs.parse_intermixed_args(sys.argv[1:])
    for words in args.words_files:
        words_f: BinaryIO
        with open(words, 'r+t') as words_f:
            old_words: list[str] = words_f.readlines()
            add_words: list[str] = []
            add_word: str
            for add_word in args.add_words:
                add_word = f'{add_word}\n'
                add_words.append(add_word)
            sorted_words_f: sorted_words = sort_words(old_words, words, add_words)
            if not args.quiet:
                sys.stdout.writelines(sorted_words_f.diff)
            if args.save and sorted_words_f.old_words != sorted_words_f.new_words:
                sys.stderr.write(f'Saving to file {words}\n')
                words_f.seek(0)
                words_f.writelines(sorted_words_f.new_words)
            words_f.close()
    sys.exit(0)
