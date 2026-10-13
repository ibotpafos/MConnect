// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "SelfSignedCertificate.h"

#include <mbedtls/asn1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <set>
#include <vector>

namespace mconnect
{
namespace
{
struct RequiredSans
{
    std::vector<std::string> dnsNames;
    std::vector<std::array<unsigned char, 4>> ipAddresses;
};

juce::String sanitizeForFilename(juce::String text)
{
    if (text.isEmpty())
        return "loopback";

    juce::String sanitized;
    for (int i = 0; i < text.length(); ++i)
    {
        const auto character = text[i];
        sanitized += juce::CharacterFunctions::isLetterOrDigit(character)
            ? juce::String::charToString(character)
            : "-";
    }

    return sanitized;
}

bool parseIpv4(const juce::String& address, std::array<unsigned char, 4>& result)
{
    const auto parts = juce::StringArray::fromTokens(address, ".", {});
    if (parts.size() != 4)
        return false;

    for (int i = 0; i < 4; ++i)
    {
        const auto text = parts[i].trim();
        const auto value = text.getIntValue();
        if (text.isEmpty() || value < 0 || value > 255)
            return false;

        result[static_cast<size_t> (i)] = static_cast<unsigned char> (value);
    }

    return true;
}

RequiredSans makeRequiredSans(const juce::String& lanIpAddress)
{
    RequiredSans required;
    required.dnsNames = { "localhost", "pigeonstream.local" };

    std::array<unsigned char, 4> ip {};
    if (parseIpv4("127.0.0.1", ip))
        required.ipAddresses.push_back(ip);

    if (parseIpv4(lanIpAddress, ip))
        required.ipAddresses.push_back(ip);

    return required;
}

juce::File certificateDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MConnect")
        .getChildFile("MConnect")
        .getChildFile("certs");
}

juce::File certificateFileFor(const juce::String& lanIpAddress)
{
    return certificateDirectory().getChildFile("mconnect-local-" + sanitizeForFilename(lanIpAddress) + ".pem");
}

std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char character)
    {
        return static_cast<char> (std::tolower(character));
    });
    return value;
}

bool certificateMatchesRequiredSans(const juce::File& file, const RequiredSans& required)
{
    if (! file.existsAsFile() || file.getSize() <= 0)
        return false;

    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);

    const auto parseResult = mbedtls_x509_crt_parse_file(&cert, file.getFullPathName().toRawUTF8());
    if (parseResult != 0)
    {
        mbedtls_x509_crt_free(&cert);
        return false;
    }

    std::set<std::string> missingDns;
    for (const auto& name : required.dnsNames)
        missingDns.insert(toLowerAscii(name));

    auto missingIps = required.ipAddresses;

    for (auto* item = &cert.subject_alt_names; item != nullptr; item = item->next)
    {
        mbedtls_x509_subject_alternative_name san;
        std::memset(&san, 0, sizeof(san));

        if (mbedtls_x509_parse_subject_alt_name(&item->buf, &san) != 0)
            continue;

        if (san.type == MBEDTLS_X509_SAN_DNS_NAME && san.san.unstructured_name.p != nullptr)
        {
            std::string dns(reinterpret_cast<const char*> (san.san.unstructured_name.p),
                            san.san.unstructured_name.len);
            missingDns.erase(toLowerAscii(std::move(dns)));
        }
        else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS
                 && san.san.unstructured_name.p != nullptr
                 && san.san.unstructured_name.len == 4)
        {
            const std::array<unsigned char, 4> found {
                san.san.unstructured_name.p[0],
                san.san.unstructured_name.p[1],
                san.san.unstructured_name.p[2],
                san.san.unstructured_name.p[3]
            };

            missingIps.erase(std::remove(missingIps.begin(), missingIps.end(), found), missingIps.end());
        }

        mbedtls_x509_free_subject_alt_name(&san);
    }

    mbedtls_x509_crt_free(&cert);
    return missingDns.empty() && missingIps.empty();
}

juce::String mbedtlsError(int error)
{
    char buffer[256] {};
    mbedtls_strerror(error, buffer, sizeof(buffer));
    return juce::String(buffer);
}

bool writeGeneratedCertificate(const juce::File& file, const RequiredSans& required, juce::String& errorText)
{
    if (! file.getParentDirectory().createDirectory())
    {
        errorText = "could not create certificate directory";
        return false;
    }

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert cert;
    mbedtls_mpi serial;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctrDrbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);
    mbedtls_mpi_init(&serial);

    auto cleanup = [&]
    {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&cert);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&ctrDrbg);
        mbedtls_entropy_free(&entropy);
    };

    const unsigned char personalisation[] = "MConnect self-signed HTTPS certificate";
    auto result = mbedtls_ctr_drbg_seed(&ctrDrbg,
                                        mbedtls_entropy_func,
                                        &entropy,
                                        personalisation,
                                        sizeof(personalisation) - 1);
    if (result != 0)
    {
        errorText = "random seed failed: " + mbedtlsError(result);
        cleanup();
        return false;
    }

    result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0)
        result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                     mbedtls_pk_ec(key),
                                     mbedtls_ctr_drbg_random,
                                     &ctrDrbg);
    if (result != 0)
    {
        errorText = "key generation failed: " + mbedtlsError(result);
        cleanup();
        return false;
    }

    std::array<unsigned char, 16> serialBytes {};
    result = mbedtls_ctr_drbg_random(&ctrDrbg, serialBytes.data(), serialBytes.size());
    if (result == 0)
        result = mbedtls_mpi_read_binary(&serial, serialBytes.data(), serialBytes.size());
    if (result != 0)
    {
        errorText = "serial generation failed: " + mbedtlsError(result);
        cleanup();
        return false;
    }

    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);

    result = mbedtls_x509write_crt_set_serial(&cert, &serial);
    if (result == 0)
        result = mbedtls_x509write_crt_set_subject_name(&cert, "CN=Pigeon Stream Local,O=MConnect");
    if (result == 0)
        result = mbedtls_x509write_crt_set_issuer_name(&cert, "CN=Pigeon Stream Local,O=MConnect");
    if (result == 0)
        result = mbedtls_x509write_crt_set_validity(&cert, "20260101000000", "20360101000000");
    if (result == 0)
        result = mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1);
    if (result == 0)
        result = mbedtls_x509write_crt_set_key_usage(&cert,
                                                     MBEDTLS_X509_KU_DIGITAL_SIGNATURE
                                                     | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);

    mbedtls_asn1_sequence serverAuth {};
    serverAuth.buf.tag = MBEDTLS_ASN1_OID;
    serverAuth.buf.p = reinterpret_cast<unsigned char*> (const_cast<char*> (MBEDTLS_OID_SERVER_AUTH));
    serverAuth.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    if (result == 0)
        result = mbedtls_x509write_crt_set_ext_key_usage(&cert, &serverAuth);

    std::vector<mbedtls_x509_san_list> sanNodes(required.dnsNames.size() + required.ipAddresses.size());
    auto nodeIndex = size_t { 0 };
    for (const auto& name : required.dnsNames)
    {
        auto& node = sanNodes[nodeIndex++];
        node.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        node.node.san.unstructured_name.p = reinterpret_cast<unsigned char*> (const_cast<char*> (name.data()));
        node.node.san.unstructured_name.len = name.size();
    }

    for (const auto& ip : required.ipAddresses)
    {
        auto& node = sanNodes[nodeIndex++];
        node.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        node.node.san.unstructured_name.p = const_cast<unsigned char*> (ip.data());
        node.node.san.unstructured_name.len = ip.size();
    }

    for (size_t i = 0; i < sanNodes.size(); ++i)
        sanNodes[i].next = i + 1 < sanNodes.size() ? &sanNodes[i + 1] : nullptr;

    if (result == 0 && ! sanNodes.empty())
        result = mbedtls_x509write_crt_set_subject_alternative_name(&cert, sanNodes.data());

    if (result != 0)
    {
        errorText = "certificate fields failed: " + mbedtlsError(result);
        cleanup();
        return false;
    }

    std::vector<unsigned char> certPem(8192);
    std::vector<unsigned char> keyPem(8192);
    result = mbedtls_x509write_crt_pem(&cert,
                                       certPem.data(),
                                       certPem.size(),
                                       mbedtls_ctr_drbg_random,
                                       &ctrDrbg);
    if (result == 0)
        result = mbedtls_pk_write_key_pem(&key, keyPem.data(), keyPem.size());
    if (result != 0)
    {
        errorText = "PEM write failed: " + mbedtlsError(result);
        cleanup();
        return false;
    }

    const juce::String pemText = juce::String::fromUTF8(reinterpret_cast<const char*> (certPem.data()))
        + "\n"
        + juce::String::fromUTF8(reinterpret_cast<const char*> (keyPem.data()));

    const auto tempFile = file.getSiblingFile(file.getFileName() + ".tmp");
    if (! tempFile.replaceWithText(pemText, false, false, "\n") || ! tempFile.moveFileTo(file))
    {
        errorText = "could not write certificate file";
        cleanup();
        return false;
    }

    cleanup();
    return true;
}
}

CertificateFiles ensureSelfSignedCertificate(const juce::String& lanIpAddress)
{
    CertificateFiles result;
    result.pemFile = certificateFileFor(lanIpAddress);
    const auto required = makeRequiredSans(lanIpAddress);

    if (certificateMatchesRequiredSans(result.pemFile, required))
    {
        result.ready = true;
        result.description = "reused self-signed certificate";
        return result;
    }

    juce::String errorText;
    if (writeGeneratedCertificate(result.pemFile, required, errorText)
        && certificateMatchesRequiredSans(result.pemFile, required))
    {
        result.ready = true;
        result.description = "generated self-signed certificate";
        return result;
    }

    result.ready = false;
    result.description = errorText.isNotEmpty() ? errorText : "certificate SAN verification failed";
    return result;
}
}
