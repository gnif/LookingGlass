#!/usr/bin/env python3
import re

from lgrelease import release

from enchant.tokenize import Filter

reacronym = re.compile(r'^[A-Z]+s?$')
reoption = re.compile(r'^[a-z]+:\w+$')
recamel = re.compile(r'^[A-Za-z]+[A-Z]\w+$')
repackage = re.compile(r'^[\w-]+-(?:dev|bin)$|^fonts-[\w-]+-ttf$|^virt-manager$')
repath = re.compile(r'^/dev/|.*\.\w+$')
recrypto = re.compile(r'^[13][A-Za-z0-9]{25,34}$|^0x[0-9a-fA-F]{40}|^4([0-9]|[A-B])(.){93}$')


class AcronymFilter(Filter):
    def _skip(self, word):
        return reacronym.match(word)


class OptionFilter(Filter):
    def _skip(self, word):
        return reoption.match(word) or recamel.match(word)


class PackageFilter(Filter):
    def _skip(self, word):
        return repackage.match(word)


class PathFilter(Filter):
    def _skip(self, word):
        return repath.match(word)


class CryptoAddressFilter(Filter):
    def _skip(self, word):
        return recrypto.match(word)


class VersionFilter(Filter):
    def _skip(self, word):
        return word == release


if __name__ == '__main__':
    import os
    import sys
    from enchant.checker import SpellChecker

    checker = SpellChecker('en_US', sys.stdin.read(), filters=[
        AcronymFilter, OptionFilter, PackageFilter, PathFilter,
    ])

    with open(os.path.join(os.path.dirname(__file__), 'words.txt')) as f:
        for line in f:
            checker.add(line.strip())

    has_error = False
    for error in checker:
        print(f'Spelling error: {error.word}')
        print(f'Context: {error.leading_context(30)}{error.word}{error.trailing_context(30)}')
        has_error = True
    sys.exit(has_error)
