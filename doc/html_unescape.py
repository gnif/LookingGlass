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


# Call html.unescape on source files when building docs
#
# This converts named HTML character references: like '&gt;' into '>',
# and numeric references: like '&#128270;' into '🔎'
#
# See https://docs.python.org/3/library/html.html#html.unescape
# and https://html.spec.whatwg.org/multipage/named-characters.html#named-character-references
# for more information on available sequences

from html import unescape

# For type hints (PEP 484)
from sphinx.application import Sphinx
from sphinx.environment import BuildEnvironment


def unescape_source(app: Sphinx, docname: str, source: list) -> None:
    env: BuildEnvironment = app.env
    exclude: list = env.config.html_unescape_exclude
    onlyinclude: list = env.config.html_unescape_onlyinclude
    docpath: str = env.doc2path(docname, base=False)
    if docname in exclude:
        return
    if docpath in exclude:
        return
    if len(onlyinclude) and not (docname in onlyinclude or docpath in onlyinclude):
        return
    source[0] = unescape(source[0])


def setup(app: Sphinx) -> dict[str, bool | str]:
    app.add_config_value('html_unescape_exclude', [], 'env', [list])
    app.add_config_value('html_unescape_onlyinclude', [], 'env', [list])
    app.connect('source-read', unescape_source)
    return {
        'version': '1.0',
        'env_version': '1.0',
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
