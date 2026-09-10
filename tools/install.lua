-- Project-owned installation records and recoverable directory replacement.
global none
global <const> require, assert, error, ipairs, pairs, type, tostring, table, utf8

local fs, hash, json, archive = require 'fs', require 'hash', require 'json', require 'archive'
local p = require 'tools.project'
local M = {}
local record_name = '.kuu-installed.json'

local function file_hash(path)
  -- Published 0.5 predates hash.file's shared long-path handling. Keep CI
  -- usable until the fixed 0.6 binary is published.
  if require('rt').version == '0.5' then
    path = p.path(path):gsub('/', '\\')
    path = path:sub(1,2) == '\\\\' and ('\\\\?\\UNC\\' .. path:sub(3)) or ('\\\\?\\' .. path)
  end
  return hash.file('sha256', path)
end

local function relative(path)
  if type(path) ~= 'string' or path == '' or not utf8.len(path)
    or path:find('[\\:%z]') or path:sub(1,1) == '/' or path:sub(-1) == '/' or path:find('//',1,true) then return false end
  for part in path:gmatch('[^/]+') do
    if part == '.' or part == '..' or part:find('[. ]$') then return false end
  end
  return true
end

-- Resolve both the spelling and the existing parent before a directory move.
local function managed(path)
  assert(relative(path) and path:sub(1,7) == '.tools/', 'installation path must be below .tools: ' .. tostring(path))
  local absolute = p.path(path)
  local parent = assert(fs.canon(fs.dirname(absolute)))
  local root = assert(fs.canon(p.root))
  local rel = assert(fs.relative(parent.path, root.path))
  assert(rel == '.' or relative(rel), 'installation parent leaves the project: ' .. absolute)
  return absolute
end

local function digest(value)
  return type(value) == 'string' and #value == 64 and value:match('^[0-9a-f]+$') ~= nil
end

function M.key(packages)
  local inputs = {}
  for _,pkg in ipairs(packages) do
    assert(type(pkg.id) == 'string' and pkg.id:match('^[%w_-]+$'), 'invalid prerequisite id')
    assert(digest(pkg.sha256) and type(pkg.url) == 'string', 'invalid prerequisite pin: ' .. pkg.id)
    assert(relative(pkg.dest) and pkg.dest:sub(1,7) == '.tools/', 'invalid prerequisite destination')
    assert(type(pkg.strip) == 'number' and pkg.strip >= 0 and pkg.strip % 1 == 0, 'invalid archive strip count')
    inputs[#inputs+1] = table.concat({pkg.id, pkg.url, pkg.sha256, pkg.dest, tostring(pkg.strip)}, '\0')
  end
  -- Order matters when upstream packages overlay the same destination.
  return hash.sum('sha256', table.concat(inputs, '\n'))
end

function M.ready(dir, key)
  if fs.exists(dir) ~= 'directory' then return false end
  local bytes = fs.read(fs.join(dir, record_name), {maxbytes='16M'})
  if not bytes then return false end
  local record = json.decode(bytes)
  if type(record) ~= 'table' or record.schema ~= 1 or record.key ~= key
    or type(record.files) ~= 'table' or #record.files == 0 then return false end
  local count = 0
  for k in pairs(record.files) do
    if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 then return false end
    count = count + 1
  end
  if count ~= #record.files then return false end
  for _,file in ipairs(record.files) do
    if type(file) ~= 'table' or not relative(file.path) or not digest(file.sha256) then return false end
    local path = fs.join(dir, file.path)
    if fs.exists(path) ~= 'file' or file_hash(path) ~= file.sha256 then return false end
  end
  return true
end

function M.record(dir, key)
  local files = {}
  local function walk(subdir)
    local listing = assert(fs.list(subdir == '' and dir or fs.join(dir, subdir)))
    assert(#listing.errors == 0, 'could not inventory ' .. dir)
    for _,entry in ipairs(listing.entries) do
      local name = subdir == '' and entry.name or fs.join(subdir, entry.name)
      assert(relative(name), 'invalid installed filename: ' .. name)
      if entry.kind == 'directory' then walk(name)
      elseif entry.kind == 'file' and name ~= record_name then
        files[#files+1] = {path=name, sha256=assert(file_hash(fs.join(dir, name)))}
      end
    end
  end
  -- Anchor the walk at the explicitly supplied root; glob would also inspect
  -- its ancestors and stop at a directory link supplied by the host sandbox.
  walk('')
  table.sort(files, function(a,b) return a.path < b.path end)
  assert(#files > 0, 'empty installation: ' .. dir)
  assert(fs.write(fs.join(dir, record_name), json.encode({schema=1, key=key, files=files})))
end

function M.ensure(dest, packages, download)
  local key = M.key(packages)
  assert(fs.mkdir(fs.dirname(dest)))
  assert(fs.mkdir('.tools/staging'))
  dest = managed(dest)
  local name = hash.sum('sha256', dest:sub(#p.root + 2):lower()):sub(1,16)
  local stage = managed('.tools/staging/' .. name)
  local backup = managed('.tools/staging/' .. name .. '-old')
  -- A kill between the two renames leaves the previous tree recoverable.
  if not fs.exists(dest) and fs.exists(backup) then assert(fs.rename(backup, dest)) end
  if M.ready(dest, key) then
    if fs.exists(stage) then p.remove(stage) end
    if fs.exists(backup) then p.remove(backup) end
    return false
  end
  if fs.exists(stage) then p.remove(stage) end
  assert(fs.mkdir(stage))
  for _,pkg in ipairs(packages) do
    local path = download(pkg)
    require('log').info('unpack ' .. pkg.id)
    assert(archive.unpack(path, stage, {strip=pkg.strip}))
  end
  M.record(stage, key)
  if fs.exists(backup) then p.remove(backup) end
  local old = fs.exists(dest)
  if old then assert(fs.rename(dest, backup)) end
  local ok, e = fs.rename(stage, dest)
  if not ok then
    if old then
      local restored, restore_error = fs.rename(backup, dest)
      if not restored then error('installation failed: ' .. tostring(e) .. '; previous tools remain at ' .. backup .. ': ' .. tostring(restore_error), 0) end
    end
    error(e, 0)
  end
  if old then p.remove(backup) end
  return true
end

return M
