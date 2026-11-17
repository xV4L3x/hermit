# Homebrew formula for hermit.
#
# This is the source of truth; publish it by copying it to the tap repo
# github.com/xV4L3x/homebrew-hermit as Formula/hermit.rb. Users then:
#
#     brew install xV4L3x/hermit/hermit
#
# It builds from one GitHub source tarball. The install is fully offline (as
# Homebrew's sandbox requires): the native engine (_libhbc) is compiled with
# cmake against Homebrew's pybind11 - CMakeLists prefers find_package over the
# FetchContent fallback, so no network is hit at configure time - and dropped
# into a virtualenv next to the pure-Python REPL (the `cli` package), which the
# `hermit` launcher runs.
#
# Finalising a release (run on a machine with Homebrew, after pushing the tag):
#   1. Point `url` at the new tag and update its sha256:
#        curl -sL https://github.com/xV4L3x/hermit/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
#   2. Generate the Python `resource` blocks for the REPL's runtime deps
#      (rich, prompt_toolkit, click, Pygments and their transitive deps). The
#      deps live in packaging/pypi/hermit/pyproject.toml, which is not the root
#      project, so point the resource tool at it, e.g. with a scratch venv:
#        python3 -m venv /tmp/rv && /tmp/rv/bin/pip install \
#          rich prompt_toolkit click Pygments
#        pipx run homebrew-pypi-poet -f rich prompt_toolkit click Pygments  # emits resource stanzas
#      then paste the stanzas where marked below.
#   3. Audit and test:
#        brew audit --strict --new Formula/hermit.rb
#        brew install --build-from-source Formula/hermit.rb
#        brew test hermit
class Hermit < Formula
  include Language::Python::Virtualenv

  desc "Hermes Reverse Engineering & Manipulation Interactive Toolkit for React Native HBC"
  homepage "https://github.com/xV4L3x/hermit"
  url "https://github.com/xV4L3x/hermit/archive/refs/tags/v0.3.0.tar.gz"
  sha256 "REPLACE_WITH_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/xV4L3x/hermit.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pybind11" => :build
  depends_on "python@3.13"

  # >>> BEGIN generated resources (step 2 above) >>>
  # resource "rich" do ... end
  # resource "prompt_toolkit" do ... end
  # resource "click" do ... end
  # resource "Pygments" do ... end
  # resource "markdown-it-py" do ... end
  # resource "mdurl" do ... end
  # resource "wcwidth" do ... end
  # <<< END generated resources <<<

  def install
    python = "python3.13"
    venv = virtualenv_create(libexec, python)

    # 1. Pure-Python runtime dependencies (the generated resource blocks).
    venv.pip_install resources

    # 2. Compile the native extension against the venv's interpreter and
    #    Homebrew's pybind11, then place it in the venv's site-packages.
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DHERMIT_BUILD_TESTS=OFF",
           "-DPython3_EXECUTABLE=#{libexec}/bin/python"
    system "cmake", "--build", "build", "--target", "_libhbc"

    sitep = libexec/"lib/#{python}/site-packages"
    sitep.install Dir["build/bindings/_libhbc*.so"]
    sitep.install "bindings/_libhbc.pyi", "bindings/py.typed"

    # 3. The pure-Python REPL package (no build backend needed - just the files),
    #    and a launcher that runs it with the venv interpreter.
    sitep.install "cli"
    (libexec/"bin/hermit").write <<~SH
      #!/bin/sh
      exec "#{libexec}/bin/python" -m cli.main "$@"
    SH
    chmod 0755, libexec/"bin/hermit"
    bin.install_symlink libexec/"bin/hermit"
  end

  test do
    assert_match "hermit #{version}", shell_output("#{bin}/hermit --version")
  end
end
