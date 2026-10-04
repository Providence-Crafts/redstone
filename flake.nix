{
  description = "redstone - a minimal SQLite shell with zsh-style completion (C99)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Sanitizer & debugging environment flags
        asanOptions = "detect_leaks=1:abort_on_error=1:symbolize=1:check_initialization_order=true:detect_stack_use_after_return=true";
        ubsanOptions = "print_stacktrace=1:halt_on_error=1";

        # The SQLite amalgamation, used by `make SQLITE=vendored` and as the
        # source of reference/shell.c. Fetched by nix, never committed to git:
        # it is 9.4 MB, and shell.c alone is 34k generated lines.
        #
        # The version must track pkgs.sqlite so that the system and vendored
        # build paths agree. Bump both together; `nix store prefetch-file
        # --unpack <url>` gives the hash.
        sqliteVersion = pkgs.sqlite.version; # 3.53.3
        sqliteAmalgamation = pkgs.fetchzip {
          url = "https://sqlite.org/2026/sqlite-amalgamation-3530300.zip";
          hash = "sha256-QgNam6cJkD3hVe8B/EZBapbneu6N23ehrgzy35egCcw=";
        };
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "redstone";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ pkgs.sqlite ];
          makeFlags = [ "PREFIX=$(out)" ];
          buildFlags = [ "release" ];
        };

        # Windows cross-build for local checks: `nix develop .#windows`, then
        # `make SQLITE=vendored release` gives a static bin/redstone.exe and
        # `make SQLITE=vendored MODE=release run-tests` runs the unit tests
        # under Wine. Releases are built on the GitHub Actions Windows runner.
        devShells.windows = pkgs.pkgsCross.mingwW64.mkShell {
          nativeBuildInputs = [ pkgs.gnumake pkgs.sqlite pkgs.wine64 ];
          # gcc's win32 thread model links mcfgthread; the cross wrapper
          # only finds it when it is a target dependency of the shell.
          buildInputs = [ pkgs.pkgsCross.mingwW64.windows.mcfgthreads ];
          env = {
            WINDOWS = "1";
            CC = "x86_64-w64-mingw32-gcc";
            TEST_WRAPPER = "wine";
            WINEDEBUG = "-all";
            SQLITE_AMALGAMATION = "${sqliteAmalgamation}";
          };
        };

        devShells.default = pkgs.mkShell {
          # nix's cc-wrapper injects -D_FORTIFY_SOURCE=2, which glibc turns
          # into a #warning at the -O0 the debug and compile_commands builds
          # use. The project treats warnings as defects, so disable the
          # hardening flag rather than teach every consumer to ignore it;
          # release builds set their own optimisation and hardening.
          hardeningDisable = [ "fortify" ];

          nativeBuildInputs = with pkgs; [
            # Compilers & Toolchains
            gcc
            clang
            llvmPackages.llvm      # llvm-symbolizer for ASan traces

            # Language Server, Formatting & Linting
            clang-tools            # clangd, clang-format, clang-tidy
            cppcheck

            # Debugging & Memory Profiling
            gdb
            lldb
            valgrind

            # Build
            gnumake
            bear                   # compile_commands.json for clangd
            pkg-config

            # Watchers & Workflow Automation
            watchexec
            entr

            # SQLite CLI: for building test fixtures and for A/B comparison
            # against the shell we are replacing.
            sqlite
          ];

          buildInputs = with pkgs; [
            sqlite                 # libsqlite3: our one real dependency
          ];

          env = {
            CC = "clang";

            ASAN_SYMBOLIZER_PATH = "${pkgs.llvmPackages.llvm}/bin/llvm-symbolizer";
            ASAN_OPTIONS = asanOptions;
            UBSAN_OPTIONS = ubsanOptions;

            # Where `make SQLITE=vendored` finds sqlite3.c / sqlite3.h, and
            # where `make reference` finds upstream shell.c.
            SQLITE_AMALGAMATION = "${sqliteAmalgamation}";
          };

          shellHook = ''
            echo "redstone dev environment"
            echo "  clang    $(clang --version | head -n1 | cut -d' ' -f1-3)"
            echo "  sqlite3  $(sqlite3 --version | cut -d' ' -f1)"
            echo "  libsqlite3 $(pkg-config --modversion sqlite3)"
            if [ "$(pkg-config --modversion sqlite3)" != "${sqliteVersion}" ]; then
              echo "  warning: amalgamation (${sqliteVersion}) and libsqlite3 disagree"
            fi
            echo ""
            echo "  make            build (system libsqlite3)"
            echo "  make SQLITE=vendored   build against the amalgamation"
            echo "  make asan       build with ASan+UBSan"
            echo "  make test       run the test suite"
            echo "  make fixtures   regenerate tests/test.db"
            echo "  make reference  fetch sqlite's shell.c into reference/"
          '';
        };
      }
    );
}
