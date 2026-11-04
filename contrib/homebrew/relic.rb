# Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
#
# SPDX-License-Identifier: BSD-2-Clause

# A Homebrew formula for relic, for a tap or for homebrew-core to adopt.
#
# The url and sha256 describe one published release tarball.  The commit
# after a release writes them, so a checksum is never a guess: until then
# sha256 is all zeros, and an install fails on the mismatch rather than
# trusting an unchecked download.
class Relic < Formula
  desc "Datalog engine for C: an embeddable library and a command-line tool"
  homepage "https://github.com/stass/relic"
  url "https://github.com/stass/relic/releases/download/v1.0.0/relic-1.0.0.tar.gz"
  sha256 "e717a79949d372005590a39c8428265ea2daeaf65a9c228f4448aa676230cb92"
  license "BSD-2-Clause"

  depends_on "pkgconf" => :test

  uses_from_macos "libedit"

  def install
    system "./configure", "--disable-silent-rules", "--with-libedit=yes",
           *std_configure_args
    system "make"
    system "make", "install"
  end

  test do
    (testpath/"graph.dl").write <<~EOS
      edge(a, b).
      edge(b, c).
      path(X, Y) :- edge(X, Y).
      path(X, Z) :- edge(X, Y), path(Y, Z).
    EOS
    assert_equal "b\nc\n", shell_output("#{bin}/relic -q 'path(a, X)?' graph.dl")

    # The library, compiled as an embedder compiles against it.
    (testpath/"query.c").write <<~EOS
      #include <stdio.h>
      #include <stdlib.h>

      #include <relic.h>

      int
      main(void)
      {
        relic_t *ctx;
        relic_result_t *result;
        int status;

        ctx = relic_create();
        if (ctx == NULL)
          return (EXIT_FAILURE);
        relic_diag_set(ctx, relic_diag_stderr, NULL);

        status = EXIT_FAILURE;
        if (relic_load_string(ctx, "test", "p(1). p(2).") == 0) {
          relic_query_t *query = relic_query_prepare(ctx, NULL, "p(X)?");

          result = query == NULL ? NULL : relic_query_collect(ctx, query);
          if (result != NULL && relic_result_rows(result) == 2)
            status = EXIT_SUCCESS;
          relic_result_destroy(result);
          relic_query_destroy(query);
        }
        printf("%s\\n", relic_version());
        relic_destroy(ctx);

        return (status);
      }
    EOS
    flags = shell_output("pkgconf --cflags --libs librelic").split
    system ENV.cc, "query.c", "-o", "query", *flags
    assert_equal "#{version}\n", shell_output("./query")
  end
end
