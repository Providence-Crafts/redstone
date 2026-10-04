# Identity proposal: new name, logo, and shared design language with librarian

Status: implemented (rev 2 decisions taken). The shared contract that came out of it is `docs/family.md`; this file is kept as the rationale.

Changes since rev 1: accent colour now follows the icon, theme is crafting/smithing/anvil/redstone, C99 is a hard constraint, and the program is renamed (binary included).

## What librarian does today (the language to inherit)

Read from `librarian/src/ui.c` and `include/ui.h`.

| Element | Rule | Inherit? |
|---|---|---|
| Logo | Isometric Minecraft block, ~28 cols x 12 rows, half-blocks (`▀ ▄`), 24-bit fg + bg, so one cell is two vertical pixels | Yes, same footprint |
| Banner layout | Block left; beside the middle rows: `glyph  S P A C E D  N A M E` (bold accent), tagline, gray facts joined by ` • ` | Yes, same positions |
| Colour roles | accent = identity, mint = counts/success, peach = in-flight, blue = tagline/separators, gray = secondary, red = error | Roles yes, hues partly |
| Prompt | `glyph name ❯ `, name bold accent, chevron secondary | Yes |
| Naming | One lowercase noun for a role | Yes |

The shared part is structure and roles. The accent hue and the block are per-program, so the two read as siblings, not twins.

## Family rule

Each program is a Minecraft villager profession; its workstation block is the logo. librarian is the Librarian (lectern/bookshelf). Your smithing, anvil and redstone ideas map onto the same rule:

| Block | Profession | Natural name | Accent from the block |
|---|---|---|---|
| Smithing table | Toolsmith | **toolsmith** | Ember orange on dark iron and charcoal: the table's black top, red-brown trim, planks |
| Anvil | (none; Blacksmith is not a station) | **anvil** | Iron slate with a hot-orange highlight |
| Blast furnace / grindstone | Armorer / Weaponsmith | armorer, weaponsmith | Furnace glow, stone grey |
| Crafting table | (none) | crafter | Plank brown and tool-blue |
| Redstone (dust, comparator, repeater) | (none) | **redstone** | Saturated red on grey stone |

Name candidates, with tradeoffs:

| Name | Block | For | Against |
|---|---|---|---|
| **toolsmith** (recommended) | Smithing table | Same register as librarian: a profession, so the family rule holds and later siblings (e.g. `cartographer`) slot in. A shell is a tool you forge queries with. Unique name, no command of this name on this machine's PATH | 9 letters to type; mitigated by a short alias if wanted |
| anvil | Anvil | Short, strong, one syllable, the anvil is the most iconic silhouette at 28x12 | Anvil is not a villager station, so it bends the family rule; "anvil" is a common project name (web frameworks, build tools), so search and packaging collisions are likely |
| redstone | Redstone circuit | Redstone is Minecraft's logic language; fits a query language best semantically, and red is a strong, distinct accent | Not a block you can show isometrically as one object (dust is flat), so the logo would be a lamp, comparator or repeater, which loses recognisability; also a Mojang-trademarked term used in many projects |
| forge / smithy | Anvil + furnace | Short | "Forge" collides with Minecraft Forge and Git forges |

Recommendation: **toolsmith**, smithing table. Accent: ember orange, roughly `232,110,60`, with a darker `160,70,40` for the trim. This contrasts with librarian's lavender and still reads as a pastel-adjacent warm hue next to it. If you prefer the anvil's silhouette, the fallback is `anvil`; the cost is the naming collisions above.

## Rename scope

You want to rename the program, binary included. The 251 occurrences of `sqlsh` sit in about 35 files. What changes and what to keep compatible:

| Surface | Action | Compatibility note |
|---|---|---|
| Binary `bin/sqlsh`, Makefile, flake `packages.default`, man page `docs/sqlsh.1`, tests | Rename to the new name | Breaking for anyone who has `sqlsh` on PATH or aliased. Optional: install a `sqlsh` symlink, off by default |
| Error prefix `sqlsh: ...` (about 30 messages) | One constant in one header, used everywhere, instead of 30 string edits | Check `tests/parity.sh`: it diffs against the real `sqlite3`, so confirm whether any case compares the prefix before changing |
| Config and data paths `~/.config/sqlsh/` (theme, history) | Move to `~/.config/<new>/`; read the old directory as a fallback | Preserves user history and theme; no silent loss |
| Header guards `SQLSH_*_H`, comments, README, ARCHITECTURE, textbook | Mechanical rename | The textbook is generated from `index.qmd`; regenerate `index.html` and `index.pdf` |
| Repo directory `~/computation/programming/c/sqlsh` | Left to you | Moving it is a filesystem decision outside the repo |

Per your guidelines this is one atomic commit separate from the branding work: rename first, then add the banner on top.

## C99

Already enforced: the Makefile builds with `-std=c99 -Wpedantic`, under POSIX X/Open as noted in the Makefile. Branding must not weaken that. Concretely:

- The banner is not a baked string of escapes like librarian's 3 KB array. It is a small indexed bitmap (`unsigned char pix[W][H]`, each value an index into a palette) plus a ~40-line renderer in plain C99 that pairs two vertical pixels into one `▀` cell. Source is small, diffable, and recolourable by the theme.
- UTF-8 block characters are written as `"\xe2\x96\x80"` escapes or via a string literal, which is valid C99 under `-pedantic` without relying on source-charset extensions.
- No `//` comments, no `typeof`, no `__attribute__` (librarian's `ATTR_PRINTF` macro is a GNU extension; do not import it), no VLAs, no `strdup`/`getline` (POSIX, not C99).
- The generator, if one is needed to draw the art, is also C99; there is no Python in the build or the repo.
- `make gate` (format, both builds, tests, parity, cppcheck, clang-tidy) stays the acceptance test.

## Colour and theme

sqlsh forbids hardcoded colour at call sites (`theme.h`): all colour is theme-file text, so `NO_COLOR`, dumb terminals and non-tty output work. The brand follows the same rule:

- New built-in theme with the ember palette, and new style slots `THEME_BRAND`, `THEME_TAGLINE` for banner and prompt.
- The block's palette indices resolve through the theme, so a user theme can recolour the logo.
- When colour is off the banner prints only the three caption lines; no grey block.

## Banner and prompt

Same 12-row footprint and caption position as librarian:

```
   (isometric smithing table, 12 rows)      ⚒  T O O L S M I T H
                                             SQLite shell, forged to fit
                                             sqlite3-compatible • Pure C99 • Zero deps
```

(Placeholder text; the art is generated from the bitmap.)

- Prompt: `⚒ toolsmith ❯ ` outside `--compat` only. Today's `sqlsh> ` prompt is part of the parity surface, and `src/line.c:329` measures prompt width with `strlen`, so the glyph must be single-width (`⚒` is ambiguous-width in some fonts; safer alternatives are `■`, `◆`, `▣`) or the editor must use `width.c`. Recommended: a single-width geometric glyph.
- The banner shows once on an interactive, non-`--compat` start, never on piped input, batch SQL or `--compat`.

## Art production

| Option | Tradeoff |
|---|---|
| A. Hand-authored 16x16-per-face palette-indexed bitmap in C source, rendered at runtime | Reproducible, editable as text, C99 only, recolourable; I draw an approximation of the smithing table, not the exact texture |
| B. Convert a reference image you supply to the indexed bitmap with a C99 converter | Closest to the real block, as with librarian; needs an image from you, and Minecraft textures are Mojang assets, so this is for a private tool, not redistribution |

Recommendation: A.

## Implementation order

1. Rename commit (scope table above), `make gate`.
2. Theme: brand styles and ember built-in theme, tests in `test_theme.c`.
3. `src/brand.c` / `include/brand.h`: bitmap, renderer, banner, prompt strings.
4. Wire into interactive non-compat startup; tests for absence in compat, batch, piped and `NO_COLOR`; prompt width test in `test_line.c`.
5. README and man page.

## Rev 3: redstone, redstone ore

Decided by you: name **redstone**, logo = redstone ore block, art from your reference renders (art option B).

- Prototype half-block renders (26x24 px, 12 rows) of both ores were made from the supplied images. The stone ore (image 2) keeps its edges on a dark terminal background; the deepslate ore (image 1) is dark grey on dark, so the cube silhouette dissolves. Recommendation: stone ore (image 2).
- Accent: redstone red, about `230,40,40` (glow highlight `255,110,110`), on stone grey. Conflict to resolve: red is also the error colour. Mitigation: errors keep their distinct style (`bold ... on` dark-red background, as in the `theme.h` example), brand red is used only for the name, glyph and logo, never for result text.
- Prompt glyph: a single-width `●` or `◆` in brand red stands in for redstone dust: `● redstone ❯ `, outside `--compat` only.
- The family rule shifts from "profession" to "block": librarian = bookshelf, redstone = redstone ore. Both are single natural blocks with a one-word lowercase name.
- Art pipeline: the reference PNGs are converted once, offline, and only the result is committed: a palette-indexed bitmap (about 16 colours) as C99 source plus the renderer. No PNG decoder and no image-tool dependency enters the repo or the build. Reference images are Mojang assets: fine for a private tool, not for redistribution.
- Name caveat: "redstone" is Mojang's term and common in unrelated projects; no command of that name is on this machine's PATH.

## Decisions needed

1. Stone ore (image 2, recommended) or deepslate ore (image 1)?
2. Config migration: `~/.config/sqlsh/` to `~/.config/redstone/` with fallback to the old directory (recommended); no `sqlsh` symlink unless you want one?
3. Prompt glyph: single-width `●`/`◆` (recommended) or an emoji with a width fix in the line editor?
