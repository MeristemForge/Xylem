#!/bin/bash
set -e
git add -A
git commit -m "chore: sync steering/build.md with docs, update gitignore and hook"
git push
