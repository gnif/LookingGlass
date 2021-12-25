# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

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

# The full version, including alpha/beta/rc tags
release = 'B4'

rst_prolog = """
.. |license| replace:: GPLv2
"""

# -- General configuration ---------------------------------------------------

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = [
]

try:
    from sphinxcontrib import spelling
except ImportError:
    pass
else:
    del spelling
    extensions += ['sphinxcontrib.spelling']

    import sys, os
    sys.path.append(os.path.dirname(__file__))
    spelling_filters = [
        'lgspell.OptionFilter', 'lgspell.PackageFilter', 'lgspell.PathFilter',
        'lgspell.CryptoAddressFilter'
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
html_theme = 'alabaster'

html_theme_options = {
    'logo': 'icon-128x128.png',
    'fixed_sidebar': 'true',
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

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['../resources/icon-128x128.png']
