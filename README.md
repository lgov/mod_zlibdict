mod_zlibdict
============

Apache module that replaces mod_deflate by offering zlib compression with a preset dictionary. This module will trigger on a new encoding, as gzip doesn't support preset dictionaries.