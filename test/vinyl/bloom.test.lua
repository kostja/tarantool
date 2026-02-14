test_run = require('test_run').new()

--
-- Setting bloom_fpr to 1 used to disable bloom filters.
-- Fuse8 filters are always built regardless of bloom_fpr,
-- so the filter is active and reflects non-existing keys.
--
s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {bloom_fpr = 1})
for i = 1, 10, 2 do s:insert{i} end
box.snapshot()
for i = 1, 10 do s:get{i} end
stat = s.index.pk:stat()
stat.disk.iterator.bloom.hit -- 5 (even keys reflected by fuse8)
stat.disk.iterator.bloom.miss -- 0
s:drop()

-- Disable tuple cache to check bloom hit/miss ratio.
box.cfg{vinyl_cache = 0}

s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {parts = {1, 'unsigned', 2, 'unsigned', 3, 'unsigned', 4, 'unsigned'}})

reflects = 0
function cur_reflects() return box.space.test.index.pk:stat().disk.iterator.bloom.hit end
function new_reflects() local o = reflects reflects = cur_reflects() return reflects - o end
seeks = 0
function cur_seeks() return box.space.test.index.pk:stat().disk.iterator.lookup end
function new_seeks() local o = seeks seeks = cur_seeks() return seeks - o end

for i = 1, 1000 do s:replace{math.ceil(i / 10), math.ceil(i / 2), i, i * 2} end
box.snapshot()

--
-- The fuse8 filter stores hashes of full keys only.  Partial-key
-- lookups (fewer parts than the index definition) skip the filter
-- entirely and always fall through to a disk seek.  Full-key
-- lookups benefit from the filter: non-existing keys are reflected
-- with high probability (fpr ~= 0.4%).
--
_ = new_reflects()
_ = new_seeks()

for i = 1, 100 do s:select{i} end
new_reflects() == 0
new_seeks() == 100

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2)} end
new_reflects() == 0
new_seeks() == 1000

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2), i} end
new_reflects() == 0
new_seeks() == 1000

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2), i, i * 2} end
new_reflects() == 0
new_seeks() == 1000

for i = 1001, 2000 do s:select{i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i, i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i, i, i} end
new_reflects() > 980 -- full key: fuse8 active
new_seeks() < 20

test_run:cmd('restart server default')

vinyl_cache = box.cfg.vinyl_cache
box.cfg{vinyl_cache = 0}

s = box.space.test

reflects = 0
function cur_reflects() return box.space.test.index.pk:stat().disk.iterator.bloom.hit end
function new_reflects() local o = reflects reflects = cur_reflects() return reflects - o end
seeks = 0
function cur_seeks() return box.space.test.index.pk:stat().disk.iterator.lookup end
function new_seeks() local o = seeks seeks = cur_seeks() return seeks - o end

_ = new_reflects()
_ = new_seeks()

for i = 1, 100 do s:select{i} end
new_reflects() == 0
new_seeks() == 100

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2)} end
new_reflects() == 0
new_seeks() == 1000

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2), i} end
new_reflects() == 0
new_seeks() == 1000

for i = 1, 1000 do s:select{math.ceil(i / 10), math.ceil(i / 2), i, i * 2} end
new_reflects() == 0
new_seeks() == 1000

for i = 1001, 2000 do s:select{i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i, i} end
new_reflects() == 0 -- partial key: fuse8 skipped
new_seeks() == 1000

for i = 1, 1000 do s:select{i, i, i, i} end
new_reflects() > 980 -- full key: fuse8 active
new_seeks() < 20

s:drop()

box.cfg{vinyl_cache = vinyl_cache}

--
-- gh-3907: check that integer numbers stored as MP_FLOAT/MP_DOUBLE
-- are hashed as MP_INT/MP_UINT.
--
ffi = require('ffi')
s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('primary', {parts = {1, 'number'}})
s:replace{ffi.new('double', 0)}
s:replace{ffi.new('double', -1)}
s:replace{ffi.new('double', 9007199254740992)}
s:replace{ffi.new('double', -9007199254740994)}
box.snapshot()
s:get(0LL)
s:get(-1LL)
s:get(9007199254740992LL)
s:get(-9007199254740994LL)
s:drop()
