// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using Microsoft.Win32.SafeHandles;

namespace System.Net
{
    internal static partial class CertificateValidationPal
    {
        internal static SslPolicyErrors VerifyCertificateProperties(
            SafeDeleteContext? _ /*securityContext*/,
            X509Chain chain,
            X509Certificate2 remoteCertificate,
            bool checkCertName,
            bool isServer,
            string? hostName)
        {
            return CertificateValidation.BuildChainAndVerifyProperties(chain, remoteCertificate, checkCertName, isServer, hostName, Span<byte>.Empty);
        }

        private static X509Certificate2? GetRemoteCertificate(
            SafeDeleteContext? securityContext,
            bool retrieveChainCertificates,
            ref X509Chain? chain,
            X509ChainPolicy? chainPolicy)
        {
            if (securityContext == null)
            {
                return null;
            }

            SafeSslHandle sslContext = ((SafeDeleteSslContext)securityContext).SslContext;

            if (sslContext == null)
            {
                return null;
            }

            X509Certificate2? result = null;

            using (SafeX509ChainHandle chainHandle = Interop.AppleCrypto.SslCopyCertChain(sslContext))
            {
                long chainSize = Interop.AppleCrypto.X509ChainGetChainSize(chainHandle);

                if (retrieveChainCertificates && chainSize > 1)
                {
                    chain ??= new X509Chain();
                    if (chainPolicy != null)
                    {
                        chain.ChainPolicy = chainPolicy;
                    }

                    // First certificate is peer's certificate.
                    // Any any additional intermediate CAs to ExtraStore.
                    for (int i = 1; i < chainSize; i++)
                    {
                        IntPtr certHandle = Interop.AppleCrypto.X509ChainGetCertificateAtIndex(chainHandle, i);
                        chain.ChainPolicy.ExtraStore.Add(new X509Certificate2(certHandle));
                    }
                }

                // This will be a distinct object than remoteCertificateStore[0] (if applicable),
                // to match what the Windows and Unix PALs do.
                if (chainSize > 0)
                {
                    IntPtr certHandle = Interop.AppleCrypto.X509ChainGetCertificateAtIndex(chainHandle, 0);
                    result = new X509Certificate2(certHandle);
                }
            }

            if (NetEventSource.Log.IsEnabled()) NetEventSource.Log.RemoteCertificate(result);

            return result;
        }

        // This is only called when we selected local client certificate.
        // Currently this is only when Apple crypto asked for it.
        internal static bool IsLocalCertificateUsed(SafeFreeCredentials? _1, SafeDeleteContext? _2) => true;

        //
        // Used only by client SSL code, never returns null.
        //
        internal static string[] GetRequestCertificateAuthorities(SafeDeleteContext securityContext)
        {
            SafeSslHandle sslContext = ((SafeDeleteSslContext)securityContext).SslContext;

            if (sslContext == null)
            {
                return Array.Empty<string>();
            }

            using (SafeCFArrayHandle dnArray = Interop.AppleCrypto.SslCopyCADistinguishedNames(sslContext))
            {
                if (dnArray.IsInvalid)
                {
                    return Array.Empty<string>();
                }

                long size = Interop.CoreFoundation.CFArrayGetCount(dnArray);

                if (size == 0)
                {
                    return Array.Empty<string>();
                }

                string[] distinguishedNames = new string[size];

                for (int i = 0; i < size; i++)
                {
                    IntPtr element = Interop.CoreFoundation.CFArrayGetValueAtIndex(dnArray, i);

                    using (SafeCFDataHandle cfData = new SafeCFDataHandle(element, ownsHandle: false))
                    {
                        byte[] dnData = Interop.CoreFoundation.CFGetData(cfData);
                        X500DistinguishedName dn = new X500DistinguishedName(dnData);
                        distinguishedNames[i] = dn.Name;
                    }
                }

                return distinguishedNames;
            }
        }

        private static X509Store OpenStore(StoreLocation storeLocation)
        {
            X509Store store = new X509Store(StoreName.My, storeLocation);
            store.Open(OpenFlags.ReadOnly);
            return store;
        }
    }
}
