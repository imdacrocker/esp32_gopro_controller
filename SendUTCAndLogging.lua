local function isLeapYear(year)
    return (year % 4 == 0 and year % 100 ~= 0) or (year % 400 == 0)
end

-- Days per month lookup (non-leap year)
local DAYS_IN_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}

local function daysInMonth(month, year)
    if month == 2 and isLeapYear(year) then
        return 29
    end
    return DAYS_IN_MONTH[month]
end

local function dateToEpochMs(year, month, day, hour, minute, second, ms)
    -- Accumulate full years since epoch
    local days = 0
    for y = 1970, year - 1 do
        days = days + (isLeapYear(y) and 366 or 365)
    end

    -- Accumulate full months in the current year
    for m = 1, month - 1 do
        days = days + daysInMonth(m, year)
    end

    -- Add remaining days in current month (day is 1-based)
    days = days + (day - 1)

    -- Convert everything to milliseconds and add sub-second component
    local totalMs = ((days * 86400) + (hour * 3600) + (minute * 60) + second) * 1000 + ms
    return totalMs
end

local function packUint64LE(val)
    local b = {}
    for i = 1, 8 do
        b[i] = math.floor(val % 256)
        val  = math.floor(val / 256)
    end
    return b
end

function sendUtc()
    local year, month, day, hour, minute, second, ms = getDateTime()

    if year <= 1970 then
        return
    end

    local epochMs = dateToEpochMs(year, month, day, hour, minute, second, ms)
    local data    = packUint64LE(epochMs)

    txCAN(0, 0x602, 0, data)
end

setTickRate(50)
function onTick()
    sendUtc()
    txCAN(0, 0x600, 0, {isLogging(), 0, 0, 0, 0, 0, 0, 0}) -- Send the isLogging
end