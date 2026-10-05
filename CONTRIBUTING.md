# contributing

a change comes with a test that fails without it. the test says which rule it checks and where the rule is written (the Direct3D specification, Microsoft's documentation, Apple's), and reads back a result: bytes, pixels, a counter. a test that only shows nothing crashed does not count.

run the suite before you send a change, with `python3 tests/run.py --build <build> --wine <wine install> --family native,9,8,7`, and say what you ran and on which Mac.

keep changes small. values come from what the API or the device says, not from tables or special cases for one game. if a game needs something, find the rule behind it.

never read or use decompiled or reverse-engineered code of D3DMetal or the Game Porting Toolkit. what we learn from Apple comes from its public documentation and from what renders.

commit messages follow DXMT's form, `<type>(<scope>): <subject>`, with a lowercase subject.

DXMT's own contribution rules are DXMT's. nothing from this repository is to be submitted to DXMT on our behalf.
