#!/bin/bash

cat > .git/hooks/pre-commit << ---
#!/bin/bash

changes=\$(git diff --cached --name-status)
m="pre-commit error"

echo "\${changes}" | while read status file; do
	# Release checks
	if [[ "\$file" =~ ^CHANGELOG/RELEASE-v.+\.md$ ]] ; then
		if ! git show ":\$file" | python3 scripts/changelog_tool.py validate --stdin --expected-version "\$(basename "\$file" .md | sed 's/^RELEASE-//')" --quiet; then
			echo "\$m release-check: invalid release file \$file" && exit 1
		fi
		echo "\${changes}" | ( ! grep -q -e '^M\\sCMakeLists.txt' ) && \\
			echo "\$m release-check: Version in CMakeLists.txt not modified!" && exit 1
		clv="\$(basename "\$file" .md | sed 's/^RELEASE-//')"
		cmv="v\$(git diff --cached CMakeLists.txt | perl -ne 'if (s/^\+project\(libosdp VERSION (\d+\.\d+\.\d+)\)/\$1/) {print;last}')"
		if [[ \$clv != "\$cmv" ]]; then
			echo "\$m release-check: Version mismatch! (\$clv/\$cmv)" && exit 1
		fi
	fi
done

files=\$(git diff  --cached --name-only | grep -E ".*\.(cpp|cc|c\+\+|cxx|c|h|hpp)$")
if [[ ! -z "\${files}" ]]; then
	git diff -U0 --cached -- \${files} | ./scripts/clang-format-diff.py -p1
fi

---
chmod a+x .git/hooks/pre-commit && echo "Installed hook: .git/hooks/pre-commit"
