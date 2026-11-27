-- SPDX-License-Identifier: GPL-2.0-or-later

local defaultTexture = "Interface\\Icons\\Ability_MeleeDamage"
local rangedSlot = GetInventorySlotInfo("RangedSlot")
local button = CreateFrame("Button", "TwowAutoAttackButton", UIParent)
button:SetWidth(36)
button:SetHeight(36)
button:SetPoint("CENTER", UIParent, "CENTER", 0, -160)
button:SetMovable(true)
button:EnableMouse(true)
button:RegisterForClicks("LeftButtonUp")
button:RegisterForDrag("RightButton")
button:SetNormalTexture(defaultTexture)
button:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")

local currentTexture = defaultTexture

local function updateIcon()
    local texture = GetInventoryItemTexture("player", rangedSlot) or defaultTexture
    if texture ~= currentTexture then
        currentTexture = texture
        button:SetNormalTexture(texture)
    end
end

local function setButtonPosition(x, y)
    button:ClearAllPoints()
    button:SetPoint("CENTER", UIParent, "CENTER", x, y)
end

button:SetScript("OnClick", function()
    if UnitExists("target") and UnitCanAttack("player", "target") then
        AttackTarget()
    end
end)

button:SetScript("OnDragStart", function() this:StartMoving() end)
button:SetScript("OnDragStop", function()
    this:StopMovingOrSizing()
    local x, y = this:GetCenter()
    local parentX, parentY = UIParent:GetCenter()
    if not x or not y or not parentX or not parentY then
        return
    end
    if type(TwowAutoAttackPosition) ~= "table" then
        TwowAutoAttackPosition = {}
    end
    TwowAutoAttackPosition[1] = x - parentX
    TwowAutoAttackPosition[2] = y - parentY
    setButtonPosition(TwowAutoAttackPosition[1], TwowAutoAttackPosition[2])
end)

button:SetScript("OnEnter", function()
    GameTooltip:SetOwner(this, "ANCHOR_RIGHT")
    GameTooltip:SetText("Adaptive Attack")
    GameTooltip:AddLine("Left-click to use Attack on your target.", 1, 1, 1)
    GameTooltip:AddLine("Right-drag to move.", 1, 1, 1)
    GameTooltip:Show()
end)
button:SetScript("OnLeave", function() GameTooltip:Hide() end)

button:RegisterEvent("PLAYER_LOGIN")
button:RegisterEvent("UNIT_INVENTORY_CHANGED")
button:SetScript("OnEvent", function()
    if event == "PLAYER_LOGIN" then
        local position = TwowAutoAttackPosition
        if type(position) == "table" and type(position[1]) == "number" and type(position[2]) == "number" then
            setButtonPosition(position[1], position[2])
        end
        updateIcon()
    elseif arg1 == "player" then
        updateIcon()
    end
end)
