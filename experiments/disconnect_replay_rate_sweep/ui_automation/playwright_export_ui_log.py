#!/usr/bin/env python3
"""
Playwright UI automation for telemetry experiments.

v4 strict changes for reconnect-only baseline:
- installs history/replay blockers before the page loads;
- also blocks fetch/XHR URLs/bodies containing history/replay/last/recent;
- removes min constraints from the Last N input and forces Last N to 0;
- protects the WebSocket port field from being overwritten by broad zero-forcing;
- prints a clear v4 banner so you can confirm the right script is running.
"""
from __future__ import annotations

import argparse
import asyncio
import re
from pathlib import Path
from typing import Optional


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Automate browser UI and export UI log CSV.")
    p.add_argument("--url", default="http://localhost:8000")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", default="8080")
    p.add_argument("--path", default="/")
    p.add_argument("--last-n", type=int, required=True)
    p.add_argument("--wait-s", type=float, default=68.0)
    p.add_argument("--output", required=True)
    p.add_argument("--headless", action="store_true")
    p.add_argument("--slow-mo-ms", type=int, default=0)
    p.add_argument("--connect-button-name", default="Connect")
    p.add_argument("--export-button-name", default="Export UI log")
    p.add_argument("--host-selector", default=None)
    p.add_argument("--port-selector", default=None)
    p.add_argument("--path-selector", default=None)
    p.add_argument("--last-n-selector", default=None)
    p.add_argument("--connect-selector", default=None)
    p.add_argument("--export-selector", default=None)
    p.add_argument("--timeout-ms", type=int, default=15000)
    p.add_argument("--block-history-requests", action="store_true")
    return p.parse_args()


BLOCKING_INIT_SCRIPT = r"""
(() => {
  if (window.__reconnectOnlyV4Installed) return;
  window.__reconnectOnlyV4Installed = true;
  window.__reconnectOnlyBlockedHistoryRequests = 0;
  window.__reconnectOnlyBlockedFetchRequests = 0;
  window.__reconnectOnlyBlockedXhrRequests = 0;
  window.__reconnectOnlySuppressedFunctions = 0;

  function isHistoryLike(s) {
    try {
      if (s === undefined || s === null) return false;
      const text = String(s).toLowerCase();
      return text.includes('history') || text.includes('replay') ||
             text.includes('last') || text.includes('recent') ||
             text.includes('batch') || text.includes('samples');
    } catch (_) { return false; }
  }

  function objectLooksHistoryLike(obj) {
    try { return isHistoryLike(JSON.stringify(obj)); }
    catch (_) { return false; }
  }

  // WebSocket request blocker.
  const originalSend = WebSocket.prototype.send;
  WebSocket.prototype.send = function(data) {
    try {
      let block = isHistoryLike(data);
      if (!block && typeof data === 'string') {
        try { block = objectLooksHistoryLike(JSON.parse(data)); } catch (_) {}
      }
      if (block) {
        window.__reconnectOnlyBlockedHistoryRequests += 1;
        console.warn('[reconnect-only v4] blocked WebSocket history/replay request:', data);
        return;
      }
    } catch (_) {}
    return originalSend.apply(this, arguments);
  };

  // fetch blocker, in case history is requested via HTTP.
  if (window.fetch) {
    const originalFetch = window.fetch.bind(window);
    window.fetch = function(input, init) {
      try {
        const url = typeof input === 'string' ? input : (input && input.url) || '';
        const body = init && init.body;
        if (isHistoryLike(url) || isHistoryLike(body) || objectLooksHistoryLike(init)) {
          window.__reconnectOnlyBlockedFetchRequests += 1;
          console.warn('[reconnect-only v4] blocked fetch history/replay request:', url, init);
          return Promise.resolve(new Response('[]', {status: 200, headers: {'Content-Type': 'application/json'}}));
        }
      } catch (_) {}
      return originalFetch(input, init);
    };
  }

  // XHR blocker.
  const originalOpen = XMLHttpRequest.prototype.open;
  const originalSendXhr = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function(method, url) {
    this.__reconnectOnlyUrl = url;
    return originalOpen.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function(body) {
    try {
      if (isHistoryLike(this.__reconnectOnlyUrl) || isHistoryLike(body)) {
        window.__reconnectOnlyBlockedXhrRequests += 1;
        console.warn('[reconnect-only v4] blocked XHR history/replay request:', this.__reconnectOnlyUrl, body);
        return;
      }
    } catch (_) {}
    return originalSendXhr.apply(this, arguments);
  };

  // Best-effort suppression for common global function names.
  const suppressNames = ['requestHistory', 'loadHistory', 'fetchHistory', 'requestReplay', 'loadReplay', 'fetchReplay'];
  for (const name of suppressNames) {
    try {
      Object.defineProperty(window, name, {
        configurable: true,
        set: function(_) {
          Object.defineProperty(window, name, {
            configurable: true,
            writable: true,
            value: function() {
              window.__reconnectOnlySuppressedFunctions += 1;
              console.warn('[reconnect-only v4] suppressed function:', name);
              return null;
            }
          });
        },
        get: function() {
          return function() {
            window.__reconnectOnlySuppressedFunctions += 1;
            console.warn('[reconnect-only v4] suppressed function:', name);
            return null;
          };
        }
      });
    } catch (_) {}
  }
})();
"""


async def fill_by_selector_or_label(page, value: str, *, selector: Optional[str], labels: list[str], regexes: list[str], timeout_ms: int) -> bool:
    candidates = []
    if selector:
        candidates.append(("selector", selector))
    for label in labels:
        candidates.append(("label", label))
    for pattern in regexes:
        candidates.append(("css", pattern))

    for kind, candidate in candidates:
        try:
            if kind == "selector":
                loc = page.locator(candidate).first
            elif kind == "label":
                loc = page.get_by_label(re.compile(candidate, re.I)).first
            else:
                loc = page.locator(candidate).first
            await loc.wait_for(state="visible", timeout=1500)
            # Remove min attribute if the requested value is 0. Some UIs have min=1 and silently coerce 0 to 1.
            if str(value) == "0":
                try:
                    await loc.evaluate("el => { el.removeAttribute('min'); el.min = '0'; }")
                except Exception:
                    pass
            await loc.fill(str(value), timeout=timeout_ms)
            try:
                await loc.evaluate(
                    "(el, value) => { el.value = String(value); el.dispatchEvent(new Event('input', {bubbles:true})); el.dispatchEvent(new Event('change', {bubbles:true})); }",
                    str(value),
                )
            except Exception:
                pass
            return True
        except Exception:
            continue
    return False


async def force_last_n_zero(page) -> int:
    """Best-effort force all likely history/replay numeric inputs to 0."""
    return await page.evaluate(r"""
    () => {
      let changed = 0;
      const inputs = Array.from(document.querySelectorAll('input'));
      for (const el of inputs) {
        const attrs = [el.id, el.name, el.placeholder, el.getAttribute('aria-label'), el.title, el.className]
          .filter(Boolean).join(' ').toLowerCase();
        const looks = attrs.includes('last') || attrs.includes('history') || attrs.includes('replay') || attrs.includes('sample');
        const numeric = el.type === 'number' || /^\d+$/.test(el.value || '');
        if (looks || numeric) {
          try {
            el.removeAttribute('min'); el.min = '0'; el.value = '0';
            el.dispatchEvent(new Event('input', {bubbles:true}));
            el.dispatchEvent(new Event('change', {bubbles:true}));
            changed += 1;
          } catch (_) {}
        }
      }
      window.__reconnectOnlyForcedLastNZero = changed;
      return changed;
    }
    """)


async def click_button(page, *, selector: Optional[str], name: str, timeout_ms: int) -> None:
    if selector:
        await page.locator(selector).first.click(timeout=timeout_ms)
        return
    attempts = [
        lambda: page.get_by_role("button", name=re.compile(name, re.I)).first,
        lambda: page.locator(f"button:has-text('{name}')").first,
        lambda: page.locator(f"input[type=button][value*='{name}'], input[type=submit][value*='{name}']").first,
        lambda: page.get_by_text(re.compile(name, re.I)).first,
    ]
    last_error = None
    for make_loc in attempts:
        try:
            loc = make_loc()
            await loc.wait_for(state="visible", timeout=2500)
            await loc.click(timeout=timeout_ms)
            return
        except Exception as exc:
            last_error = exc
    raise RuntimeError(f"Could not click button/text matching {name!r}. Last error: {last_error}")


async def main_async(args: argparse.Namespace) -> None:
    from playwright.async_api import async_playwright, TimeoutError as PlaywrightTimeoutError
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    print("[UI automation v4 strict] Starting")
    async with async_playwright() as pw:
        browser = await pw.chromium.launch(headless=args.headless, slow_mo=args.slow_mo_ms)
        context = await browser.new_context(accept_downloads=True)
        if args.block_history_requests:
            print("[UI automation v4 strict] Installing pre-load history/replay blockers")
            await context.add_init_script(BLOCKING_INIT_SCRIPT)
            # Also abort obvious HTTP history/replay calls. This does not affect WebSocket itself.
            async def route_handler(route):
                req = route.request
                u = req.url.lower()
                if any(k in u for k in ['history', 'replay', 'last', 'recent']):
                    print(f"[UI automation v4 strict] Aborting HTTP history/replay request: {req.url}")
                    await route.fulfill(status=200, body='[]', headers={'Content-Type':'application/json'})
                else:
                    await route.continue_()
            await context.route("**/*", route_handler)
        page = await context.new_page()
        page.set_default_timeout(args.timeout_ms)

        print(f"[UI automation v4 strict] Opening {args.url}")
        await page.goto(args.url, wait_until="domcontentloaded")
        if args.block_history_requests:
            # Install again after page load in case page scripts overwrote prototypes.
            await page.evaluate(BLOCKING_INIT_SCRIPT)

        # Fill Last N first. Some UIs coerce 0 to 1 through min=1; when requested, force
        # likely replay/history numeric inputs to 0. Then fill host/port/path LAST so the
        # broad zero-forcing cannot accidentally change the WebSocket port to 0.
        lastn_ok = await fill_by_selector_or_label(page, str(args.last_n), selector=args.last_n_selector,
            labels=["last n", "last n samples", "replay window", "history window", "history samples"],
            regexes=["input[name*=last i]", "input[name*=history i]", "input[name*=replay i]", "#lastN", "#last-n", "#historyCount", "#replayWindow", "input[placeholder*=Last i]"], timeout_ms=args.timeout_ms)
        forced = 0
        if args.last_n == 0:
            forced = await force_last_n_zero(page)

        host_ok = await fill_by_selector_or_label(page, args.host, selector=args.host_selector,
            labels=["host", "server host", "ws host"], regexes=["input[name*=host i]", "#host", "input[placeholder*=Host i]"], timeout_ms=args.timeout_ms)
        port_ok = await fill_by_selector_or_label(page, args.port, selector=args.port_selector,
            labels=["port", "ws port", "websocket port"], regexes=["input[name*=port i]", "#port", "input[placeholder*=Port i]"], timeout_ms=args.timeout_ms)
        path_ok = await fill_by_selector_or_label(page, args.path, selector=args.path_selector,
            labels=["path", "ws path", "websocket path"], regexes=["input[name*=path i]", "#path", "input[placeholder*=Path i]"], timeout_ms=args.timeout_ms)

        # Verify the page did not keep port=0 after zero-forcing.
        page_ws_values = await page.evaluate(r"""
        () => {
          const vals = {};
          const inputs = Array.from(document.querySelectorAll('input'));
          for (const el of inputs) {
            const attrs = [el.id, el.name, el.placeholder, el.getAttribute('aria-label'), el.title, el.className]
              .filter(Boolean).join(' ').toLowerCase();
            if (attrs.includes('port')) vals.port = el.value;
            if (attrs.includes('last') || attrs.includes('history') || attrs.includes('replay') || attrs.includes('sample')) vals.lastN = el.value;
          }
          return vals;
        }
        """)

        print(f"[UI automation v4 strict] Filled controls: host={host_ok}, port={port_ok}, path={path_ok}, last_n={lastn_ok}, forced_zero_inputs={forced}, page_values={page_ws_values}")
        if not lastn_ok:
            raise RuntimeError("Could not find/fill the Last N / replay-window input. Pass --last-n-selector.")

        print("[UI automation v4 strict] Clicking Connect")
        await click_button(page, selector=args.connect_selector, name=args.connect_button_name, timeout_ms=args.timeout_ms)

        print(f"[UI automation v4 strict] Waiting {args.wait_s:.1f} s for the run to complete")
        await asyncio.sleep(args.wait_s)
        if args.block_history_requests:
            stats = await page.evaluate(r"""() => ({
              ws: window.__reconnectOnlyBlockedHistoryRequests || 0,
              fetch: window.__reconnectOnlyBlockedFetchRequests || 0,
              xhr: window.__reconnectOnlyBlockedXhrRequests || 0,
              fn: window.__reconnectOnlySuppressedFunctions || 0,
              forced: window.__reconnectOnlyForcedLastNZero || 0
            })""")
            print(f"[UI automation v4 strict] Blocked/suppressed history/replay: {stats}")

        print(f"[UI automation v4 strict] Exporting UI log to {output}")
        try:
            async with page.expect_download(timeout=args.timeout_ms) as download_info:
                await click_button(page, selector=args.export_selector, name=args.export_button_name, timeout_ms=args.timeout_ms)
            download = await download_info.value
            await download.save_as(str(output))
        except PlaywrightTimeoutError:
            print("[UI automation v4 strict] No browser download detected; trying CSV fallback")
            csv_text = await page.evaluate(r"""
            () => {
              const ta = Array.from(document.querySelectorAll('textarea')).find(x => x.value && x.value.includes('wall_ms,event'));
              if (ta) return ta.value;
              const pre = Array.from(document.querySelectorAll('pre')).find(x => x.innerText && x.innerText.includes('wall_ms,event'));
              if (pre) return pre.innerText;
              const a = Array.from(document.querySelectorAll('a[download]')).find(x => x.href && x.href.startsWith('data:'));
              if (a) return decodeURIComponent(a.href.split(',').slice(1).join(','));
              if (window.uiLogCsv) return window.uiLogCsv;
              if (window.exportedCsv) return window.exportedCsv;
              return null;
            }
            """)
            if not csv_text:
                raise RuntimeError("Export failed: no download and no CSV text found.")
            output.write_text(csv_text, encoding="utf-8")

        await context.close()
        await browser.close()

    if not output.exists() or output.stat().st_size == 0:
        raise RuntimeError(f"Export failed: {output} was not created or is empty")
    print(f"[UI automation v4 strict] Saved {output} ({output.stat().st_size} bytes)")


def main() -> None:
    args = parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
