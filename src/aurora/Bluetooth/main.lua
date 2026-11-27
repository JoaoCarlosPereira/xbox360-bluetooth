scriptTitle = "Bluetooth"
scriptAuthor = "bt360"
scriptVersion = 1
scriptDescription = "Mostra os avisos do adaptador Bluetooth."
scriptPermissions = { "filesystem" }

local seen = 0

local function show_pending()
    local raw = FileSystem.ReadFile("Usb:\\bt360_notice.txt")
    if raw == nil or #raw < 10 then
        return
    end
    local seq = tonumber(string.sub(raw, 1, 8))
    local text = string.match(raw, "^%d+ (.-)\r?\n")
    if seq == nil or text == nil or text == "" or seq == seen then
        return
    end
    seen = seq
    Script.ShowNotification(text, 0)
end

function main()
    Script.SetStatus("Avisos do Bluetooth")
    while not Script.IsCanceled() do
        show_pending()
        Thread.Sleep(400)
    end
end
