-- Delete odd keys from 1 to 1000000
-- DEBUG POPULATE creates keys without zero-padding: key:1, key:2, etc.
local deleted = 0
for i = 1, 1000000, 2 do
  local key = string.format("key:%d", i)
  if redis.call('DEL', key) == 1 then
    deleted = deleted + 1
  end

  -- Progress indicator every 100k
  if deleted % 100000 == 0 then
    redis.log(redis.LOG_NOTICE, string.format("Deleted %d keys", deleted))
  end
end
return deleted
