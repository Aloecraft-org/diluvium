-- Test 1: Basic encrypted function
~function SecureAdd(a, b)
    print("Computing: " .. a .. " + " .. b)
    return a + b
end

-- Test 2: Encrypted function with multiple strings
~function SecureGreet(name)
    local prefix = "Hello, "
    local suffix = "! Welcome to the encrypted zone."
    return prefix .. name .. suffix
end

-- Test 3: Local encrypted function
local ~function SecureSecret()
    local password = "super_secret_password_123"
    local key = "encryption_key_xyz"
    print("This function holds secrets!")
    return password, key
end

-- Test 4: Nested functions (only outer is encrypted)
~function OuterSecure()
    print("Outer function is encrypted")
    
    function InnerNotSecure()
        print("Inner function is NOT encrypted")
    end
    
    InnerNotSecure()
end

-- Test 5: Normal unencrypted function for comparison
function NormalFunction()
    print("This is a normal unencrypted function")
    return "visible_string"
end

-- Run tests
print("=== Running Encryption Tests ===")
print("Result:", SecureAdd(5, 3))
print("Greeting:", SecureGreet("Alice"))

local p, k = SecureSecret()
print("Secrets retrieved:", p, k)

OuterSecure()
NormalFunction()

print("=== All tests complete ===")