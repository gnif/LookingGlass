import re

from enchant.tokenize import Filter

reoption = re.compile(r'^[a-z]+:\w+$')
recamel = re.compile(r'^[a-z]+[A-Z]\w+$')
repackage = re.compile(r'^[\w-]+-(?:dev|bin)$|^fonts-[\w-]+-ttf$|^virt-manager$')
repath = re.compile(r'^/dev/')
recrypto = re.compile(r'^[13][A-Za-z0-9]{25,34}$|^0x[0-9a-fA-F]{40}$')


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
