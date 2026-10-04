# The README demo. Regenerate from the repository root with
#   make release && nix shell nixpkgs#vhs nixpkgs#sqlite -c vhs docs/demo.tape
# It types into a real shell against a fresh copy of the test fixtures.

Output docs/demo.gif

Set Shell bash
Set FontSize 16
Set Width 1200
Set Height 700
Set Padding 20
Set Theme "Dracula"
Set TypingSpeed 45ms
Set PlaybackSpeed 1.0

Hide
Type "export PATH=$PWD/bin:$PATH COLORTERM=truecolor LANG=C.UTF-8 LC_ALL=C.UTF-8 PS1='$ '"
Enter
Type "cd $(mktemp -d) && sqlite3 shop.db < $OLDPWD/tests/fixtures.sql && clear"
Enter
Sleep 1s
Show

Type "redstone shop.db"
Sleep 400ms
Enter
Sleep 1.5s

# Tab opens the completion menu; arrows move it, Enter accepts.
Type "SELECT * FROM "
Sleep 300ms
Tab
Sleep 1s
Right
Sleep 400ms
Right
Sleep 600ms
Enter
Type "LIMIT 4;"
Sleep 500ms
Enter
Sleep 2s

# A unique prefix completes in place, no menu.
Type "SELECT d.name, count(*) staff FROM emp"
Tab
Type "e JOIN dep"
Tab
Type "d ON d.id = e.dept_id GROUP BY 1;"
Sleep 500ms
Enter
Sleep 2.5s

Type ".mode json"
Enter
Type "SELECT name, building FROM departments LIMIT 2;"
Enter
Sleep 2.5s

Ctrl+D
Sleep 500ms
