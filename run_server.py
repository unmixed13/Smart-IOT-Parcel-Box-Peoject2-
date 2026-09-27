"""
run_server.py — Windows-safe launcher for the Smart IoT Parcel Box backend.

Use this instead of `python -m uvicorn app.main:app ...` on Windows.

Why this file exists:
  asyncio's default event loop on Windows (ProactorEventLoop, the default
  since Python 3.8) does not implement add_reader()/add_writer(), which
  aiomqtt's underlying socket handling requires. That raises:

      NotImplementedError
      File "...asyncio\\events.py", line 538, in add_reader

  The fix is to switch to SelectorEventLoop instead — but that switch must
  happen BEFORE uvicorn creates its event loop (asyncio.run() picks up
  whatever policy is active at the moment it's called). Doing this inside
  app/main.py is too late, because uvicorn's own startup sequence creates
  the loop first and only imports app.main afterwards. This tiny launcher
  sets the policy first, then hands off to uvicorn.

Usage:
    python run_server.py

This is what the NSSM service ("ParcelBoxServer") should run instead of
`-m uvicorn app.main:app ...` directly.
"""
import sys

if sys.platform == "win32":
    import asyncio
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

import uvicorn

if __name__ == "__main__":
    uvicorn.run(
        "app.main:app",
        host="0.0.0.0",
        port=8000,
        reload=False,  # never use --reload under NSSM/a service
    )
