#!/usr/bin/env bash
set -euo pipefail

# dry-run if CLEAN=false; set CLEAN=true to apply changes
CLEAN=${CLEAN:-true}
BACKUP_DIR=${BACKUP_DIR:-.diff_cleanup_backups}
SYNTAX_CHECK=${SYNTAX_CHECK:-true}
GIT_BRANCH=${GIT_BRANCH:-fix/remove-diff-markers}

mkdir -p "$BACKUP_DIR"

echo "Scanning repository for files containing unified-diff/patch markers..."
files=$(git grep -l -E '^(--- |\+\+\+ |@@ )' || true)
if [ -z "$files" ]; then
  echo "No files with diff markers found."
  exit 0
fi

echo "Found files:"
printf ' - %s\n' $files

# Do a safe dry-run first: show snippets
for f in $files; do
  echo "==== $f (context around markers) ===="
  git --no-pager -p -U0 -- "$f" | sed -n '1,200p'
  echo
done

if [ "$CLEAN" != "true" ]; then
  echo "CLEAN is false; exiting after dry-run. Re-run with CLEAN=true to apply."
  exit 0
fi

# Clean each file
for f in $files; do
  echo "Cleaning: $f"
  # backup current file
  ts=$(date +%s)
  backup="$BACKUP_DIR/$(basename "$f").bak.$ts"
  mkdir -p "$(dirname "$backup")"
  cp -- "$f" "$backup"
  echo " - backup: $backup"

  # Apply perl-based cleanup:
  # 1) remove diff header lines (--- a/..., +++ b/..., @@ ...)
  # 2) drop lines that start with '-' (old removed lines)
  # 3) remove leading '+' from added lines
  # This preserves untouched context lines.
  tmpfile="$(mktemp)"
  perl -0777 -pe '
    # Remove unified diff headers (---, +++, @@ lines)
    s/^--- .*?\n\+\+\+ .*?\n@@.*?\n//sm;
    # Remove lines that start with '-' (they are removed in the diff)
    s/^\-.*\n//mg;
    # Remove leading + on added lines
    s/^\+//mg;
  ' "$f" > "$tmpfile"

  # Trim potential leading/trailing blank lines
  # (optional) keep as is to avoid altering file semantics too much

  if ! mv "$tmpfile" "$f"; then
    echo "ERROR: failed to replace $f"
    rm -f "$tmpfile"
    exit 1
  fi
done

# Optional quick C++ syntax check: run g++ -fsyntax-only on src files that changed
if [ "$SYNTAX_CHECK" = "true" ]; then
  echo "Running quick syntax checks..."
  changed_cpp_files=$(git ls-files --modified --others --exclude-standard | grep -E '\\.(cpp|cc|cxx|hpp|h)$' || true)
  # if not using git modified list (we haven't committed yet), find all cpp files that contain no null bytes
  changed_cpp_files=$(printf '%s\n' $files | grep -E '\\.(cpp|cc|cxx|hpp|h)$' || true)
  if [ -n "$changed_cpp_files" ]; then
    for f in $changed_cpp_files; do
      echo " - syntax check: $f"
      if ! g++ -fsyntax-only -std=c++17 -Iinclude "$f"; then
        echo "Syntax check failed for $f; restore from backup or inspect manually: $BACKUP_DIR/$(basename "$f").bak.*"
        exit 2
      fi
    done
  else
    echo "No C/C++ files to syntax-check."
  fi
fi

# Stage and commit the changes
echo "Staging changes..."
git add $files

msg="Remove accidental unified-diff/patch markers from source files"
git commit -m "$msg" || { echo "Nothing to commit or commit failed."; exit 0; }

echo "Committed on branch $(git rev-parse --abbrev-ref HEAD)."
echo "You can push with: git push origin HEAD:$GIT_BRANCH"
echo "If you want me to create a PR from $GIT_BRANCH to the target branch, run that locally or give me permission to push."

echo "Done."
