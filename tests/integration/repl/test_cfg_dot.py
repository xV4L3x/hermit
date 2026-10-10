"""cli/cfg_dot.py - DOT generation and the graph-easy dependency check."""

import pytest

import _libhbc
from cli import cfg_dot
from testutil import corpus_path


def _cfg_and_func(corpus_name="hello.hbc", func_idx=1):
    hbc = _libhbc.HBCFile.open(str(corpus_path(corpus_name)))
    func = hbc.functions()[func_idx]
    return hbc.cfg(func_idx), func


def test_cfg_to_dot_produces_digraph():
    cfg, func = _cfg_and_func()
    dot = cfg_dot.cfg_to_dot(cfg, func)
    assert dot.startswith("digraph ")
    assert f"B{cfg.entry_id}" in dot


def test_render_cfg_raises_without_graph_easy(monkeypatch):
    """graph-easy is a required external dependency (Perl/CPAN) for `cfg` -
    missing it must fail loudly, never fall back to dumping raw DOT."""
    cfg, func = _cfg_and_func()
    monkeypatch.setattr(cfg_dot, "_find_graph_easy", lambda: None)
    with pytest.raises(RuntimeError, match="graph-easy"):
        cfg_dot.render_cfg(cfg, func)
