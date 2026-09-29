/* Lua preset runtime on fengari (Lua 5.3 in JS). A preset script runs as a
 * coroutine; every host op (rotate/wait/...) yields once, which is the unit
 * of single-stepping. Blocking ops (wait) stay pending: each tick the runtime
 * advances them (host.step), rebuilds the observation, and re-evaluates the
 * script's predicate until it holds or the op times out. The host interface
 * is engine-agnostic, so the same runtime shape can be reimplemented over the
 * live vision pipeline.
 *
 * Host interface:
 *   ops: { name: {
 *     desc(args) -> string,          // status line
 *     run?(args),                    // immediate op
 *     start?(args) -> pending,       // blocking op
 *     step?(pending, dt),            // advance one frame
 *   } }
 *   buildObs() -> plain observation object handed to script predicates.
 * Predicates are Lua functions; the runtime wraps them as JS callables
 * (obs) -> bool evaluated on a separate thread.
 */
(function (root) {
  'use strict';
  const fengari = root.fengari;
  if (!fengari) throw new Error('fengari-web.js must be loaded first');
  const { lua, lauxlib, lualib, to_luastring, to_jsstring } = fengari;

  /* Predicate + helper library injected before the user script. Predicates
   * read the observation table the host passes in each evaluation. */
    const BASE_LIB = `
-- Beam predicate registry: beam.terminal/hits note which entity id they aim
-- at (__ALIGN), so observation tooling can tell what a condition watches.
__ALIGN = {}
local function _register(fn, id)
  __ALIGN[fn] = id
  return fn
end
beam = {}
function beam.hits(id)
  return _register(function(obs)
    for _, e in ipairs(obs.beam.chain) do
      if e == id then return true end
    end
    return false
  end, id)
end
-- terminal = the beam's last segment is associated with id: either the beam
-- ends on that target/firewall, or id is the LAST STEERABLE MIRROR the beam
-- touches. After that mirror the beam may still bounce off fixed triangle
-- reflectors (no selectable id), so "last chain entry" is not the criterion.
function beam.terminal(id)
  return _register(function(obs)
    return obs.beam.terminal == id or obs.beam.lastMirror == id
  end, id)
end
function destroyed(id)
  return function(obs)
    local e = obs.entities[id]
    return e ~= nil and e.alive == false
  end
end
function selected(id)
  return function(obs)
    local m = obs.mirrors[id]
    return m ~= nil and m.selected == true
  end
end
-- combinators: any(p1, p2), all(p1, p2) -- plain "or"/"and" would just pick
-- the first function object (functions are truthy in Lua), not combine them.
-- Alignment metadata passes through: the first predicate carrying a target
-- id decides what align() aims at.
function any(...)
  local ps = {...}
  local target = nil
  for _, p in ipairs(ps) do
    if target == nil and __ALIGN[p] ~= nil then target = __ALIGN[p] end
  end
  return _register(function(obs)
    for _, p in ipairs(ps) do
      if p(obs) then return true end
    end
    return false
  end, target)
end
function all(...)
  local ps = {...}
  local target = nil
  for _, p in ipairs(ps) do
    if target == nil and __ALIGN[p] ~= nil then target = __ALIGN[p] end
  end
  return _register(function(obs)
    for _, p in ipairs(ps) do
      if not p(obs) then return false end
    end
    return true
  end, target)
end
`;

  function errText(L) {
    const s = lua.lua_tostring(L, -1);
    return s ? to_jsstring(s) : String(fengari.interop.tojs(L, -1));
  }

  class LuaRuntime {
    constructor(ops, buildObs) {
      this.ops = ops;
      this.buildObs = buildObs;
      this.speed = 1;
      this.clock = 0;      // speed-scaled script time, seconds
      this.running = false;
      this.finished = false;
      this.error = null;
      this.pending = null;   // blocking op currently pumped
      this.currentDesc = ''; // status line of the op being executed
      this.resumeReturn = null; // values to hand back to the next resume
      this.onStatus = null;  // UI callbacks
      this.onError = null;
      this.onDone = null;
      this.onPause = null;
      this.pauseAfterOp = false;

      this.L = lauxlib.luaL_newstate();
      lualib.luaL_openlibs(this.L);
      this.thread = lua.lua_newthread(this.L);
      this.eval = lua.lua_newthread(this.L); // predicate evaluation thread
      this._registerOps();
    }

    /* ---- Lua <-> JS conversion ---- */
    _toJs(L, idx) {
      const t = lua.lua_type(L, idx);
      switch (t) {
        case lua.LUA_TNIL: return null;
        case lua.LUA_TBOOLEAN: return !!lua.lua_toboolean(L, idx);
        case lua.LUA_TNUMBER: return lua.lua_tonumber(L, idx);
        case lua.LUA_TSTRING: return to_jsstring(lua.lua_tostring(L, idx));
        case lua.LUA_TFUNCTION: return this._wrapFunction(L, idx);
        case lua.LUA_TTABLE: return this._tableToJs(L, idx);
        default: return null;
      }
    }

    _tableToJs(L, idx) {
      const abs = lua.lua_absindex(L, idx);
      const out = {};
      lua.lua_pushnil(L);
      while (lua.lua_next(L, abs) !== 0) {
        const key = this._toJs(L, -2);
        out[key === null ? '_' : key] = this._toJs(L, -1);
        lua.lua_pop(L, 1);
      }
      return out;
    }

    _wrapFunction(L, idx) {
      lua.lua_pushvalue(L, idx);
      const ref = lauxlib.luaL_ref(L, lua.LUA_REGISTRYINDEX);
      const evalL = this.eval;
      return (obs) => {
        lua.lua_rawgeti(evalL, lua.LUA_REGISTRYINDEX, ref);
        this._pushJs(evalL, obs);
        if (lua.lua_pcall(evalL, 1, 1, 0) !== lua.LUA_OK) {
          const msg = errText(evalL);
          lua.lua_pop(evalL, 1);
          throw new Error('谓词执行失败: ' + msg);
        }
        const v = !!lua.lua_toboolean(evalL, -1); // fengari returns a JS boolean
        lua.lua_pop(evalL, 1);
        return v;
      };
    }

    _pushJs(L, v) {
      if (v === null || v === undefined) { lua.lua_pushnil(L); return; }
      switch (typeof v) {
        case 'boolean': lua.lua_pushboolean(L, v ? 1 : 0); return;
        case 'number': lua.lua_pushnumber(L, v); return;
        case 'string': lua.lua_pushstring(L, to_luastring(v)); return;
      }
      if (Array.isArray(v)) {
        lua.lua_createtable(L, v.length, 0);
        v.forEach((el, i) => {
          this._pushJs(L, el);
          lua.lua_rawseti(L, -2, i + 1);
        });
        return;
      }
      lua.lua_createtable(L, 0, Object.keys(v).length);
      for (const k of Object.keys(v)) {
        lua.lua_pushstring(L, to_luastring(k));
        this._pushJs(L, v[k]);
        lua.lua_rawset(L, -3);
      }
    }

    _args(L) {
      const n = lua.lua_gettop(L);
      const out = [];
      for (let i = 1; i <= n; i++) out.push(this._toJs(L, i));
      return out;
    }

    /* ---- host op registration ---- */
    _registerOps() {
      for (const name of Object.keys(this.ops)) {
        const spec = this.ops[name];
        lua.lua_pushcfunction(this.L, (L) => {
          const args = this._args(L);
          this.currentDesc = spec.desc(args);
          this._emitStatus();
          if (spec.start) {
            this.pending = spec.start(args);
            this.pending.op = name;
            if (this.pending.failed) {
              this._fail(name + '(' + (this.pending.argsText || '') + '): ' + this.pending.failed);
              return lua.lua_yield(L, 0);
            }
            if (this.pending.timeout) this.pending.deadline = this.clock + this.pending.timeout;
          } else {
            const ret = spec.run(args);
            if (ret && ret.length) this.resumeReturn = ret; // handed back on next resume
          }
          return lua.lua_yield(L, 0);
        });
        lua.lua_setglobal(this.L, to_luastring(name));
      }
    }

    /* ---- lifecycle ---- */
    start(code) {
      this.finished = false;
      this.error = null;
      this.pending = null;
      this.resumeReturn = null;
      this.pauseAfterOp = false;
      this.clock = 0;
      if (lauxlib.luaL_dostring(this.L, to_luastring(BASE_LIB)) !== lua.LUA_OK) {
        this._fail('内置库错误: ' + errText(this.L));
        return;
      }
      if (lauxlib.luaL_loadbuffer(this.L, to_luastring(code), null, to_luastring('preset')) !== lua.LUA_OK) {
        this._fail('脚本加载失败: ' + errText(this.L));
        return;
      }
      // move the loaded chunk into the coroutine thread
      lua.lua_xmove(this.L, this.thread, 1);
      this.running = true;
      this._emitStatus('已启动');
      this.tick(0); // run up to the first yield immediately
    }

    pause() {
      this.running = false;
      this._emitStatus('已暂停');
      if (this.onPause) this.onPause();
    }

    resumeRun() { this.running = true; this.pauseAfterOp = false; this._emitStatus('运行中'); }

    stepOnce() {
      this.pauseAfterOp = true;
      this.running = true;
      this._emitStatus('单步…');
    }

    stop() {
      this.running = false;
      this.finished = true;
      this.pending = null;
      this._emitStatus('已停止');
    }

    _fail(msg) {
      this.error = msg;
      this.running = false;
      this.finished = true;
      this.pending = null;
      if (this.onError) this.onError(msg);
      this._emitStatus('错误');
    }

    /* Advance one frame. dt is real seconds; script clock and blocking ops
     * run at this.speed. */
    tick(dt) {
      if (!this.running || this.finished) return;
      const sdt = dt * this.speed;
      this.clock += sdt;

      if (this.pending) {
        if (!this._pump(this.pending, sdt)) return;
        if (this.pauseAfterOp) { this.pauseAfterOp = false; this.pause(); return; }
        this._resume();
        return;
      }
      this._resume();
    }

    _pump(p, sdt) {
      const spec = this.ops[p.op];
      if (spec.step) spec.step(p, sdt);
      /* Completion: an explicit done flag wins; otherwise the predicate
       * decides -- align checks it first too ("already on target: do
       * nothing") and only falls back to turning when it fails. */
      let ok;
      if (p.done) {
        ok = true;
      } else if (p.cond) {
        try {
          ok = p.cond(this.buildObs());
        } catch (e) {
          this._fail(p.op + ': ' + e.message);
          return false;
        }
      } else {
        ok = false; // no cond and not done: keep pumping
      }
      if (ok) {
        if (spec.finish) spec.finish(p);
        this.pending = null;
        return true;
      }
      if (p.exhausted) {
        this._fail(p.op + '(' + (p.argsText || '') + '): ' + (p.failText || '搜索一圈仍未满足条件'));
        return false;
      }
      if (p.deadline !== undefined && this.clock >= p.deadline) {
        this._fail(p.op + '(' + (p.argsText || '') + '): 超时 ' + p.timeout + 's');
        return false;
      }
      this._emitStatus();
      return false;
    }

    _resume() {
      let narg = 0;
      if (this.resumeReturn) {
        for (const v of this.resumeReturn) this._pushJs(this.thread, v);
        narg = this.resumeReturn.length;
        this.resumeReturn = null;
      }
      const rc = lua.lua_resume(this.thread, this.L, narg);
      if (rc === lua.LUA_OK) {
        this.running = false;
        this.finished = true;
        this._emitStatus('已完成');
        if (this.onDone) this.onDone();
        return;
      }
      if (rc !== lua.LUA_YIELD) {
        this._fail('脚本错误: ' + errText(this.thread));
        return;
      }
      lua.lua_settop(this.thread, 0); // discard yielded values
      if (this.pauseAfterOp && !this.pending) {
        this.pauseAfterOp = false;
        this.pause();
      }
    }

    _emitStatus(force) {
      if (this.onStatus) {
        const pendingText = this.pending ? this._pendingText() : '';
        this.onStatus(force || this.currentDesc, pendingText, this.clock);
      }
    }

    _pendingText() {
      const p = this.pending;
      let t = '';
      if (p.deadline !== undefined) {
        t = ' ' + Math.max(0, p.deadline - this.clock).toFixed(1) + 's';
      }
      return this.currentDesc + t;
    }
  }

  root.Sim = root.Sim || {};
  root.Sim.LuaRuntime = LuaRuntime;
})(globalThis);
