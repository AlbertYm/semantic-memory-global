#include "memory/memory_security.h"
#include "foundation/str_util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char buffer[64];
    size_t buffer_length;
} security_sha256_t;

static uint32_t rotr32(uint32_t value, unsigned int shift) {
    return (value >> shift) | (value << (32U - shift));
}

static void sha256_transform(security_sha256_t *ctx, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        size_t offset = i * 4;
        words[i] = ((uint32_t)block[offset] << 24U) | ((uint32_t)block[offset + 1] << 16U) |
                   ((uint32_t)block[offset + 2] << 8U) | (uint32_t)block[offset + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 =
            rotr32(words[i - 15], 7U) ^ rotr32(words[i - 15], 18U) ^ (words[i - 15] >> 3U);
        uint32_t s1 = rotr32(words[i - 2], 17U) ^ rotr32(words[i - 2], 19U) ^ (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t sum1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
        uint32_t sum0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(security_sha256_t *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->buffer_length = 0;
}

static void sha256_update(security_sha256_t *ctx, const unsigned char *data, size_t length) {
    ctx->bit_count += (uint64_t)length * 8U;
    while (length > 0) {
        size_t available = sizeof(ctx->buffer) - ctx->buffer_length;
        size_t take = length < available ? length : available;
        memcpy(ctx->buffer + ctx->buffer_length, data, take);
        ctx->buffer_length += take;
        data += take;
        length -= take;
        if (ctx->buffer_length == sizeof(ctx->buffer)) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_length = 0;
        }
    }
}

static void sha256_finish(security_sha256_t *ctx, unsigned char digest[32]) {
    ctx->buffer[ctx->buffer_length++] = 0x80U;
    if (ctx->buffer_length > 56) {
        while (ctx->buffer_length < 64)
            ctx->buffer[ctx->buffer_length++] = 0;
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_length = 0;
    }
    while (ctx->buffer_length < 56)
        ctx->buffer[ctx->buffer_length++] = 0;
    for (size_t i = 0; i < 8; i++) {
        ctx->buffer[63 - i] = (unsigned char)(ctx->bit_count >> (i * 8U));
    }
    sha256_transform(ctx, ctx->buffer);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (unsigned char)(ctx->state[i] >> 24U);
        digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16U);
        digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8U);
        digest[i * 4 + 3] = (unsigned char)ctx->state[i];
    }
}

static void content_sha256(const char *content, size_t length,
                           char output[CBM_MEMORY_SECURITY_SHA256_HEX_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    security_sha256_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)content, length);
    sha256_finish(&ctx, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        output[i * 2] = hex[digest[i] >> 4U];
        output[i * 2 + 1] = hex[digest[i] & 0x0fU];
    }
    output[64] = '\0';
}

static bool utf8_at(const unsigned char *input, size_t length, size_t offset, unsigned char a,
                    unsigned char b, unsigned char c) {
    return offset + 2 < length && input[offset] == a && input[offset + 1] == b &&
           input[offset + 2] == c;
}

static char *normalize_for_detection(const char *content, size_t length) {
    const unsigned char *input = (const unsigned char *)content;
    char *output = malloc(length + 1);
    if (!output)
        return NULL;
    size_t out = 0;
    for (size_t i = 0; i < length;) {
        if (utf8_at(input, length, i, 0xe2U, 0x80U, 0x8bU) ||
            utf8_at(input, length, i, 0xe2U, 0x80U, 0x8cU) ||
            utf8_at(input, length, i, 0xe2U, 0x80U, 0x8dU) ||
            utf8_at(input, length, i, 0xe2U, 0x81U, 0xa0U) ||
            utf8_at(input, length, i, 0xefU, 0xbbU, 0xbfU)) {
            i += 3;
            continue;
        }
        if (utf8_at(input, length, i, 0xe3U, 0x80U, 0x80U)) {
            output[out++] = ' ';
            i += 3;
            continue;
        }
        if (i + 2 < length && input[i] == 0xefU &&
            (input[i + 1] == 0xbcU || input[i + 1] == 0xbdU)) {
            uint32_t codepoint = input[i + 1] == 0xbcU ? 0xff00U + (uint32_t)(input[i + 2] - 0x80U)
                                                       : 0xff40U + (uint32_t)(input[i + 2] - 0x80U);
            if (codepoint >= 0xff01U && codepoint <= 0xff5eU) {
                output[out++] = (char)(codepoint - 0xff01U + 0x21U);
                i += 3;
                continue;
            }
        }
        unsigned char value = input[i++];
        output[out++] = value < 0x80U ? (char)tolower(value) : (char)value;
    }
    output[out] = '\0';
    return output;
}

static bool contains_any(const char *text, const char *const *needles, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strstr(text, needles[i]))
            return true;
    }
    return false;
}

static bool has_digit_run(const char *text, size_t required, bool require_phone_prefix) {
    for (size_t i = 0; text[i];) {
        if (!isdigit((unsigned char)text[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (isdigit((unsigned char)text[i]))
            i++;
        size_t run = i - start;
        if (run == required &&
            (!require_phone_prefix ||
             (text[start] == '1' && text[start + 1] >= '3' && text[start + 1] <= '9'))) {
            return true;
        }
    }
    return false;
}

static bool has_personal_email(const char *text) {
    const char *at = text;
    while ((at = strchr(at, '@')) != NULL) {
        const char *domain = at + 1;
        const char *end = domain;
        while (*end && (isalnum((unsigned char)*end) || *end == '.' || *end == '-'))
            end++;
        size_t domain_length = (size_t)(end - domain);
        if (domain_length > 0 && !(domain_length == strlen("example.com") &&
                                   strncmp(domain, "example.com", domain_length) == 0)) {
            return true;
        }
        at++;
    }
    return false;
}

static int base64_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z')
        return (int)(value - 'A');
    if (value >= 'a' && value <= 'z')
        return (int)(value - 'a') + 26;
    if (value >= '0' && value <= '9')
        return (int)(value - '0') + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

static bool decoded_base64_has_prompt(const char *text, size_t length) {
    for (size_t start = 0; start < length;) {
        while (start < length && base64_value((unsigned char)text[start]) < 0)
            start++;
        size_t end = start;
        while (end < length && (base64_value((unsigned char)text[end]) >= 0 || text[end] == '='))
            end++;
        if (end - start >= 24) {
            size_t capacity = ((end - start) / 4 + 1) * 3 + 1;
            char *decoded = calloc(capacity, 1);
            if (!decoded)
                return false;
            size_t out = 0;
            for (size_t i = start; i + 3 < end; i += 4) {
                int a = base64_value((unsigned char)text[i]);
                int b = base64_value((unsigned char)text[i + 1]);
                int c = text[i + 2] == '=' ? 0 : base64_value((unsigned char)text[i + 2]);
                int d = text[i + 3] == '=' ? 0 : base64_value((unsigned char)text[i + 3]);
                if (a < 0 || b < 0 || c < 0 || d < 0)
                    break;
                decoded[out++] = (char)(((unsigned int)a << 2U) | ((unsigned int)b >> 4U));
                if (text[i + 2] != '=') {
                    decoded[out++] =
                        (char)((((unsigned int)b & 0x0fU) << 4U) | ((unsigned int)c >> 2U));
                }
                if (text[i + 3] != '=') {
                    decoded[out++] = (char)((((unsigned int)c & 0x03U) << 6U) | (unsigned int)d);
                }
            }
            decoded[out] = '\0';
            for (size_t i = 0; i < out; i++) {
                if ((unsigned char)decoded[i] < 0x20U && decoded[i] != '\t' && decoded[i] != '\n') {
                    decoded[i] = ' ';
                } else if ((unsigned char)decoded[i] < 0x80U) {
                    decoded[i] = (char)tolower((unsigned char)decoded[i]);
                }
            }
            bool found = strstr(decoded, "ignore previous") != NULL ||
                         strstr(decoded, "execute tool") != NULL ||
                         strstr(decoded, "system prompt") != NULL;
            free(decoded);
            if (found)
                return true;
        }
        start = end > start ? end : start + 1;
    }
    return false;
}

static void set_result(cbm_memory_security_result_t *out, bool allowed, const char *code,
                       const char *category, const char *action, const char *reason) {
    out->allowed = allowed;
    out->code = code;
    out->category = category;
    out->action = action;
    out->reason_code = reason;
}

int cbm_memory_security_scan(const char *content, size_t content_length,
                             cbm_memory_security_result_t *out) {
    if (!content || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->content_length = content_length;
    content_sha256(content, content_length, out->content_sha256);
    if (content_length > CBM_MEMORY_SECURITY_MAX_INPUT_BYTES) {
        set_result(out, false, "SECURITY_POLICY_MISMATCH", "policy", "reject",
                   "input_size_exceeded");
        return 0;
    }
    char *text = normalize_for_detection(content, content_length);
    if (!text)
        return -1;

    static const char *const credential_labels[] = {
        "api_key=",  "api_key:", "api-key=", "api-key:",      "password=",
        "password:", "passwd=",  "passwd:",  "access_token=", "access_token:",
    };
    if (strstr(text, "authorization:") && strstr(text, "bearer ")) {
        set_result(out, false, "SECURITY_SECRET_REJECTED", "credential_secret", "reject",
                   "bearer_token");
    } else if (strstr(text, "-----begin") && strstr(text, "private key-----")) {
        set_result(out, false, "SECURITY_SECRET_REJECTED", "credential_secret", "reject",
                   "private_key_material");
    } else if ((strstr(text, "server=") || strstr(text, "data source=")) &&
               (strstr(text, "password=") || strstr(text, "pwd="))) {
        set_result(out, false, "SECURITY_SECRET_REJECTED", "credential_secret", "reject",
                   "connection_string_credential");
    } else if (contains_any(text, credential_labels,
                            sizeof(credential_labels) / sizeof(credential_labels[0]))) {
        set_result(out, false, "SECURITY_SECRET_REJECTED", "credential_secret", "reject",
                   "credential_assignment");
    } else if (has_digit_run(text, 18, false)) {
        set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "direct_pii", "reject",
                   "national_identifier");
    } else if (has_digit_run(text, 16, false)) {
        set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "direct_pii", "reject",
                   "payment_card");
    } else if (has_digit_run(text, 11, true)) {
        set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "direct_pii", "reject",
                   "precise_phone");
    } else if (has_personal_email(text)) {
        set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "direct_pii", "reject",
                   "personal_email");
    } else {
        static const char *const tool_directives[] = {
            "execute tool", "call tool", "执行工具", "\"tool\"", "\\\"tool\\\"",
        };
        bool tool_directive = contains_any(text, tool_directives,
                                           sizeof(tool_directives) / sizeof(tool_directives[0]));
        bool data_exfiltration =
            (strstr(text, "export") || strstr(text, "exfiltrat") || strstr(text, "leak data")) &&
            (strstr(text, "memory") || strstr(text, "data"));
        bool system_exfiltration =
            (strstr(text, "reveal") || strstr(text, "leak") || strstr(text, "泄露")) &&
            (strstr(text, "system prompt") || strstr(text, "系统提示"));
        bool override = strstr(text, "ignore previous") || strstr(text, "ignore all previous") ||
                        strstr(text, "忽略以前") || strstr(text, "忽略之前");
        if (tool_directive) {
            set_result(out, false, "SECURITY_PROMPT_INJECTION_REJECTED", "prompt_control_injection",
                       "reject", "tool_execution_directive");
        } else if (data_exfiltration) {
            set_result(out, false, "SECURITY_PROMPT_INJECTION_REJECTED", "prompt_control_injection",
                       "reject", "data_exfiltration");
        } else if (system_exfiltration) {
            set_result(out, false, "SECURITY_PROMPT_INJECTION_REJECTED", "prompt_control_injection",
                       "reject", "system_prompt_exfiltration");
        } else if (override) {
            set_result(out, false, "SECURITY_PROMPT_INJECTION_REJECTED", "prompt_control_injection",
                       "reject",
                       (strstr(text, "system") || strstr(text, "系统")) ? "rule_override"
                                                                        : "prompt_injection");
        } else if (decoded_base64_has_prompt(content, content_length)) {
            set_result(out, false, "SECURITY_PROMPT_INJECTION_REJECTED", "prompt_control_injection",
                       "reject", "prompt_injection");
        } else if (strstr(text, "named_person=")) {
            set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "indirect_pii",
                       "needs_confirmation", "named_person");
        } else if (strstr(text, "named_organization=")) {
            set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "indirect_pii",
                       "needs_confirmation", "named_organization");
        } else if (strstr(text, "precise_location=")) {
            set_result(out, false, "SECURITY_PII_CONFIRMATION_REQUIRED", "indirect_pii",
                       "needs_confirmation", "precise_location");
        } else {
            set_result(out, true, "OK", "safe_ordinary", "allow_untrusted",
                       "ordinary_code_or_fact");
        }
    }
    free(text);
    return 0;
}

bool cbm_memory_security_scope_allowed(const char *project, const char *store) {
    if (!project || !project[0] || !store || strcmp(store, "project-memory") != 0)
        return false;
    if (strstr(project, "..") || strchr(project, '/') || strchr(project, '\\') ||
        strchr(project, ':')) {
        return false;
    }
    return cbm_validate_project_name(project);
}

bool cbm_memory_security_injection_allowed(const char *classifier_status,
                                           const char *classification) {
    return classifier_status && classification && strcmp(classifier_status, "pass") == 0 &&
           strcmp(classification, "safe") == 0;
}
