import os.path
import sys

sys.path.append(os.path.abspath('..'))
sys.path.append(os.path.abspath('exts'))

master_doc = 'index'

extensions = [
    'autopackage',
    'setuptools_config',
    'sphinx.ext.autodoc',
    'sphinx.ext.extlinks',
    'sphinx.ext.intersphinx',
    'sphinx.ext.viewcode',
]

autodoc_mock_imports = ['_drgn']

extlinks = {
    'linux': ('https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/%s', ''),
}

intersphinx_mapping = {
    'python': ('https://docs.python.org/3', None),
}

manpages_url = 'http://man7.org/linux/man-pages/man{section}/{page}.{section}.html'

html_theme = 'alabaster'

html_theme_options = {
    'description': 'Debugger-as-a-library',
    'github_user': 'osandov',
    'github_repo': 'drgn',
    'github_button': True,
    'github_type': 'star',
}