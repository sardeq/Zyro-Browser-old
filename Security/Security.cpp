#include "Security.h"
#include "Globals.h"
#include "Utils.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/aes.h>

#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
    #include <wincrypt.h>
    #pragma comment(lib, "crypt32.lib")
#else
    #include <glib/gstdio.h>
    #include <libsecret/secret.h>
#endif

// ============================================================================
// CROSS-PLATFORM CRYPTO UTILS
// ============================================================================

void generate_random_key(unsigned char* buffer, int size) { 
    RAND_bytes(buffer, size); 
}

std::string hex_encode(const unsigned char* data, int len) {
    std::stringstream ss;
    for (int i = 0; i < len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return ss.str();
}

std::vector<unsigned char> hex_decode(const std::string& input) {
    std::vector<unsigned char> output;
    for (size_t i = 0; i < input.length(); i += 2) {
        std::string byteString = input.substr(i, 2);
        output.push_back((unsigned char)strtol(byteString.c_str(), NULL, 16));
    }
    return output;
}

std::string encrypt_string(const std::string& plain) {
    if (!global_security.ready) return "";
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, global_security.key, global_security.iv);
    
    std::vector<unsigned char> ciphertext(plain.length() + AES_BLOCK_SIZE);
    int len, ciphertext_len;
    
    EVP_EncryptUpdate(ctx, ciphertext.data(), &len, (unsigned char*)plain.c_str(), plain.length());
    ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
    ciphertext_len += len;
    
    EVP_CIPHER_CTX_free(ctx);
    return hex_encode(ciphertext.data(), ciphertext_len);
}

std::string decrypt_string(const std::string& hex_cipher) {
    if (!global_security.ready) return "";
    std::vector<unsigned char> ciphertext = hex_decode(hex_cipher);
    std::vector<unsigned char> plaintext(ciphertext.size() + AES_BLOCK_SIZE);
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, global_security.key, global_security.iv);
    
    int len, plaintext_len;
    EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size());
    plaintext_len = len;
    
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    EVP_CIPHER_CTX_free(ctx);
    plaintext_len += len;
    
    return std::string((char*)plaintext.data(), plaintext_len);
}

// ============================================================================
// OS-SPECIFIC SECURITY IMPLEMENTATION
// ============================================================================

#ifdef _WIN32
// ---- Windows ---------------------------------------------------------------

void init_security() {
    std::string key_file_path = get_user_data_dir() + "os_key.bin";
    
    // 1. Try to load existing key
    if (std::ifstream in{key_file_path, std::ios::binary | std::ios::ate}) {
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        
        std::vector<char> encrypted_data(size);
        if (in.read(encrypted_data.data(), size)) {
            DATA_BLOB in_blob;
            in_blob.pbData = (BYTE*)encrypted_data.data();
            in_blob.cbData = (DWORD)size;
            
            DATA_BLOB out_blob;
            
            // Decrypt using Windows User Credentials
            if (CryptUnprotectData(&in_blob, NULL, NULL, NULL, NULL, 0, &out_blob)) {
                if (out_blob.cbData == 32) {
                    memcpy(global_security.key, out_blob.pbData, 32);
                    global_security.ready = true;
                    LocalFree(out_blob.pbData);
                }
            }
        }
    }

    // 2. If Failed or Not Found -> Create New
    if (!global_security.ready) {
        generate_random_key(global_security.key, 32);
        
        DATA_BLOB in_blob;
        in_blob.pbData = global_security.key;
        in_blob.cbData = 32;
        
        DATA_BLOB out_blob;
        // Encrypt using Windows User Credentials
        if (CryptProtectData(&in_blob, L"ZyroBrowserMasterKey", NULL, NULL, NULL, 0, &out_blob)) {
            std::ofstream out(key_file_path, std::ios::binary);
            out.write((char*)out_blob.pbData, out_blob.cbData);
            out.close();
            LocalFree(out_blob.pbData);
            global_security.ready = true;
        }
    }
    
    for(int i = 0; i < 16; i++) {
        global_security.iv[i] = global_security.key[i] ^ 0xAA;
    }
}

void reset_security_and_exit() {
    // On Windows the AES key is stored as a DPAPI-encrypted local file.
    // Deleting that file is the entire reset — there is no keyring entry.
    std::string key_path = get_user_data_dir() + "os_key.bin";
    std::string pw_path  = get_user_data_dir() + "passwords.txt";

    auto del = [](const std::string& path, const char* label) {
        if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
            if (DeleteFileA(path.c_str()))
                fprintf(stdout, "[reset-security] Deleted %s\n", label);
            else
                fprintf(stderr, "[reset-security] Could not delete %s — remove it manually.\n", label);
        } else {
            fprintf(stdout, "[reset-security] %s not present (already absent).\n", label);
        }
    };

    del(key_path, "os_key.bin");
    del(pw_path,  "passwords.txt");

    fprintf(stdout,
        "\nSecurity reset complete.\n"
        "A new key will be generated on the next launch.\n"
        "Saved passwords have been cleared.\n"
    );
    std::exit(0);
}

#else
// ---- Linux / macOS ---------------------------------------------------------

const SecretSchema* get_zyro_schema(void) {
    static const SecretSchema schema = {
        "org.freedesktop.Secret.Generic", SECRET_SCHEMA_NONE,
        { 
            { "application", SECRET_SCHEMA_ATTRIBUTE_STRING }, 
            { NULL, SECRET_SCHEMA_ATTRIBUTE_STRING } 
        }
    };
    return &schema;
}

void init_security() {
    GError *error = NULL;
    gchar *stored_key_hex = secret_password_lookup_sync(get_zyro_schema(), NULL, &error, "application", "zyro_browser_master_key", NULL);
    
    if (stored_key_hex != NULL) {
        std::vector<unsigned char> raw = hex_decode(std::string(stored_key_hex));
        if (raw.size() == 32) { 
            memcpy(global_security.key, raw.data(), 32); 
            global_security.ready = true; 
        }
        secret_password_free(stored_key_hex);
    }
    
    if (!global_security.ready) {
        generate_random_key(global_security.key, 32);
        std::string hex_key = hex_encode(global_security.key, 32);
        secret_password_store_sync(get_zyro_schema(), SECRET_COLLECTION_DEFAULT, "Zyro Browser Master Key", hex_key.c_str(), NULL, &error, "application", "zyro_browser_master_key", NULL);
        
        if (!error) {
            global_security.ready = true;
        } else {
            g_error_free(error);
        }
    }
    
    for(int i = 0; i < 16; i++) {
        global_security.iv[i] = global_security.key[i] ^ 0xAA;
    }
}

void reset_security_and_exit() {
    GError* err = nullptr;

    // Removes every keyring item matching the schema + attribute pair.
    gboolean cleared = secret_password_clear_sync(
        get_zyro_schema(),
        nullptr,                              // cancellable
        &err,
        "application", "zyro_browser_master_key",
        nullptr                               // sentinel
    );

    if (err) {
        // Keyring daemon not running, or entry already gone — not fatal.
        fprintf(stderr, "[reset-security] libsecret: %s\n", err->message);
        g_error_free(err);
    } else if (cleared) {
        fprintf(stdout, "[reset-security] Keyring entry 'zyro_browser_master_key' cleared.\n");
    } else {
        fprintf(stdout, "[reset-security] No keyring entry found (already absent).\n");
    }

    // Delete the passwords file.
    std::string pw_path = get_user_data_dir() + "passwords.txt";
    if (g_file_test(pw_path.c_str(), G_FILE_TEST_EXISTS)) {
        if (g_unlink(pw_path.c_str()) == 0)
            fprintf(stdout, "[reset-security] passwords.txt deleted.\n");
        else
            fprintf(stderr, "[reset-security] Could not delete passwords.txt\n"
                            "                 Remove it manually: %s\n", pw_path.c_str());
    } else {
        fprintf(stdout, "[reset-security] passwords.txt not present (already absent).\n");
    }

    fprintf(stdout,
        "\nSecurity reset complete.\n"
        "A fresh AES-256 key will be generated and stored in your keyring\n"
        "on the next normal launch of Zyro.\n"
    );
    std::exit(0);
}

#endif