# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Release import

import sys, os
sys.path.append(os.path.dirname(__file__))

from lgrelease import release

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))

# -- Project information -----------------------------------------------------

project = 'Looking Glass'
copyright = '2021, Looking Glass team'
author = 'Geoffrey McRae and the Looking Glass team'

rst_prolog = """
.. |license| replace:: GPLv2
"""

# -- General configuration ---------------------------------------------------

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.

extensions = [
    'sphinx_rtd_theme',
]

try:
    from sphinxcontrib import spelling
except ImportError:
    pass
else:
    del spelling
    extensions += ['sphinxcontrib.spelling']

    spelling_filters = [
        'lgspell.OptionFilter', 'lgspell.PackageFilter', 'lgspell.PathFilter',
        'lgspell.CryptoAddressFilter', 'lgspell.VersionFilter'
    ]
    spelling_word_list_filename = [os.path.join(os.path.dirname(__file__), 'words.txt')]

# Add any paths that contain templates here, relative to this directory.
# templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

# Explicitly state master_doc instead of relying on default
master_doc = 'index'

# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'sphinx_rtd_theme'

html_theme_options = {
    'logo_only': True,
    'style_nav_header_background': '#343131',
}

html_sidebars = {
    '**': [
        'about.html',
        'navigation.html',
        'relations.html',
        'searchbox.html',
    ]
}

html_favicon = '../resources/icon.ico'

html_logo = '../resources/icon-128x128.png'

html_css_files = [
    'center-rtd.css',
]

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = [
    'css/center-rtd.css',
]
