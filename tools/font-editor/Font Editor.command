#!/bin/zsh -l
# Double-click to start the font editor. Edits save to fonts/cubevox-fonts.json.

cd "${0:A:h}" || exit 1
exec node serve.js
