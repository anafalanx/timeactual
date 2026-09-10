global none
global <const> require, assert, ipairs, print, table, tostring

local fs, hash = require 'fs', require 'hash'
local p = require 'tools.project'
local install = require 'tools.install'
local M = {}

local function posix(path)
  return p.capture {p.path('.tools/msys2/usr/bin/cygpath.exe'), '-u', p.path(path)}:gsub('%s+$', '')
end

function M.ensure(packages)
  -- Formatting/reordering pins must not rebuild Tcl/Tk. Software inputs and
  -- recipe changes do; rebuild in fresh directories so old .o's cannot survive.
  local inputs = {}
  for _,pkg in ipairs(packages) do
    if pkg.group == 'build' then inputs[#inputs+1] = pkg.id .. ':' .. pkg.sha256 end
  end
  table.sort(inputs)
  local fingerprint = hash.sum('sha256', table.concat(inputs, '\n')) .. assert(hash.file('sha256', 'tools/tcltk.lua'))
  local needed = {p.tcl, p.tcls, p.wish, '.tools/tcltk/tcl9s/bin/wish90s.exe',
    '.tools/tcltk/tcl9/include/tcl.h', '.tools/tcltk/tcl9/include/tk.h',
    '.tools/tcltk/tcl9s/lib/libtcl90.a', '.tools/tcltk/tcl9s/lib/libtcl9tk90.a',
    '.tools/tcltk/tcl9/lib/tcl9.0/init.tcl', '.tools/tcltk/tcl9/lib/tk9.0/tk.tcl'}
  local ready = install.ready('.tools/tcltk', fingerprint)
  for _,file in ipairs(needed) do if fs.exists(file) ~= 'file' then ready = false end end
  if ready then return end
  -- A failed or interrupted build must never retain a completion record.
  if fs.exists('.tools/tcltk/.kuu-installed.json') then p.remove('.tools/tcltk/.kuu-installed.json') end
  local bash = p.path('.tools/msys2/usr/bin/bash.exe')
  local make = p.path('.tools/msys2/usr/bin/make.exe')
  local jobs = tostring(require('sys').info().cpus)
  for _,kind in ipairs({'shared', 'static'}) do
    local prefix = kind == 'static' and '.tools/tcltk/tcl9s' or '.tools/tcltk/tcl9'
    local mode = kind == 'static' and '--disable-shared' or '--enable-shared'
    for _,name in ipairs({'tcl', 'tk'}) do
      local dir = '.tools/build/' .. name .. '-' .. kind
      if fs.exists(dir) then p.remove(dir) end
      assert(fs.mkdir(dir))
      print('configure ' .. name .. ' 9.0.3 (' .. kind .. ')')
      local cmd = {bash, '--noprofile', '--norc', posix('.tools/src/' .. name .. '/win/configure'),
        '--enable-64bit', mode, '--prefix=' .. posix(prefix), 'CC=gcc', 'CFLAGS=-O2',
        'LDFLAGS=-static-libgcc', cwd=dir, timeout='10m'}
      if name == 'tk' then cmd[#cmd+1] = '--with-tcl=' .. posix('.tools/build/tcl-' .. kind) end
      p.exec(cmd)
      p.exec {make, '-j' .. jobs, cwd=dir, timeout='20m'}
      local targets = name == 'tcl' and {'install-binaries', 'install-libraries', 'install-headers'}
        or {'install-binaries', 'install-libraries'}
      local installcmd = p.args({make}, targets)
      installcmd.cwd = dir
      p.exec(installcmd)
    end
  end
  for _,file in ipairs(needed) do p.need(file) end
  install.record('.tools/tcltk', fingerprint)
end

return M
