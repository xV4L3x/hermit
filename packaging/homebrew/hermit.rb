# Homebrew formula for hermit.
#
# This is the source of truth; publish it by copying it to the tap repo
# github.com/xV4L3x/homebrew-tap as Formula/hermit.rb. Users then:
#
#     brew install xV4L3x/tap/hermit
#   or: brew tap xV4L3x/tap && brew install hermit
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

  desc "Reverse-engineer and patch Hermes bytecode from React Native apps"
  homepage "https://github.com/xV4L3x/hermit"
  url "https://github.com/xV4L3x/hermit/archive/refs/tags/v0.3.0.tar.gz"
  sha256 "5a39c36c7a0bea97d689f342fdfe70448d344c13fcbba405ad9715f8eb2a08b1"
  license "MIT"
  head "https://github.com/xV4L3x/hermit.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pybind11" => :build
  depends_on "python@3.13"

  # >>> BEGIN generated resources (regenerate with the step-2 recipe above) >>>
  resource "click" do
    url "https://files.pythonhosted.org/packages/45/37/edbf1b0707709013c0cf2e8317e03d97ed29bf4cac709fe328904ae92a38/click-8.5.1.tar.gz"
    sha256 "9bbfada76703fd1d784b83eea012e3e8c6a001a876543ef104fa56aaaeb072b4"
  end

  resource "markdown-it-py" do
    url "https://files.pythonhosted.org/packages/16/fc/cee2c45553c9a9bf77d8b0c391560c556d7ed756b1f0bb35eb5d48db09ea/markdown_it_py-4.3.0.tar.gz"
    sha256 "91a58fa5b72c71a40cf7177250fd767480d7a2bb3ac426ccfb76fb90305fdaaa"
  end

  resource "mdurl" do
    url "https://files.pythonhosted.org/packages/d6/54/cfe61301667036ec958cb99bd3efefba235e65cdeb9c84d24a8293ba1d90/mdurl-0.1.2.tar.gz"
    sha256 "bb413d29f5eea38f31dd4754dd7377d4465116fb207585f97bf925588687c1ba"
  end

  resource "prompt-toolkit" do
    url "https://files.pythonhosted.org/packages/7d/ea/39b988c938f75cb75d7045b5c69f8bfed47ee2152c8837fb403de29d6fb8/prompt_toolkit-3.0.53.tar.gz"
    sha256 "9ec8a0ad96d5c56148b3f914aa79c1564c3fde5d2e6b876e7bc327e353cf8fa6"
  end

  resource "Pygments" do
    url "https://files.pythonhosted.org/packages/49/2e/ced460408999b33da6b31b0021b0f37d329e202d4169aeb164493778f25b/pygments-2.21.0.tar.gz"
    sha256 "610ca751c9bc2492b38eb9a38a7fbc93edbbb2d7182edaf34e66ae493dee5c8c"
  end

  resource "rich" do
    url "https://files.pythonhosted.org/packages/c0/8f/0722ca900cc807c13a6a0c696dacf35430f72e0ec571c4275d2371fca3e9/rich-15.0.0.tar.gz"
    sha256 "edd07a4824c6b40189fb7ac9bc4c52536e9780fbbfbddf6f1e2502c31b068c36"
  end

  resource "wcwidth" do
    url "https://files.pythonhosted.org/packages/ea/01/e5ac23733628a6bbb91913a765fcf55ee8070d8d991bc88fdaaf805052c7/wcwidth-0.9.4.tar.gz"
    sha256 "fd8d57d3cdfe570e3265c8fab1885c24e34ef40889c0759c0c304a19e9835954"
  end
  # <<< END generated resources <<<

  def install
    venv = virtualenv_create(libexec, "python3.13")

    # 1. Pure-Python runtime dependencies (the generated resource blocks).
    venv.pip_install resources

    # 2. Compile the native extension against the venv's interpreter and
    #    Homebrew's pybind11, then place it in the venv's site-packages.
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DHERMIT_BUILD_TESTS=OFF",
           "-DPython3_EXECUTABLE=#{libexec}/bin/python"
    system "cmake", "--build", "build", "--target", "_libhbc"

    sitep = libexec/Language::Python.site_packages(libexec/"bin/python")
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
