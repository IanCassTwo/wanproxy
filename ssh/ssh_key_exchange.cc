/*
 * Copyright (c) 2012 Juli Mallett. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <openssl/bn.h>
#include <openssl/dh.h>

#include <common/buffer.h>

#include <crypto/crypto_hash.h>

#include <event/event_callback.h>

#include <ssh/ssh_algorithm_negotiation.h>
#include <ssh/ssh_key_exchange.h>
#include <ssh/ssh_protocol.h>
#include <ssh/ssh_server_host_key.h>
#include <ssh/ssh_session.h>
#include <ssh/ssh_filter.h>

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// File:           ssh_key_exchange.cc                                        //
// Description:    SSH key check performed on filter session start            //
// Project:        WANProxy XTech                                             //
// Adapted by:     Andreu Vidal Bramfeld-Software                             //
// Last modified:  2015-04-01                                                 //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#define	DH_GROUP_MIN	1024
#define	DH_GROUP_MAX	8192

#define	USE_TEST_GROUP

namespace {
#ifdef USE_TEST_GROUP
	static uint8_t test_prime_and_generator[] = {
		0x00, 0x00, 0x00, 0x81, 0x00, 0xe3, 0x1d, 0xfe, 0x85, 0x59, 0x9b, 0xcb, 0x5c, 0x2b, 0xbe, 0xcf,
		0x20, 0x1f, 0x5f, 0x49, 0xf1, 0xea, 0x31, 0x07, 0x7d, 0xa9, 0x26, 0xcb, 0x31, 0x03, 0x9d, 0x82,
		0x33, 0x2f, 0xed, 0x67, 0xa3, 0xa9, 0xb1, 0xc9, 0xe6, 0x34, 0x6c, 0xd7, 0xb5, 0x1a, 0x0a, 0x94,
		0x11, 0xa7, 0xd9, 0x26, 0xff, 0x0e, 0x8d, 0x72, 0xc1, 0x7b, 0x53, 0x9a, 0x13, 0x78, 0x7e, 0x16,
		0x38, 0x74, 0x7c, 0xb2, 0xdc, 0x60, 0x2c, 0x8c, 0xe8, 0x31, 0xf8, 0xd9, 0x7b, 0xac, 0xa6, 0x71,
		0xee, 0x61, 0x0c, 0x1a, 0xa4, 0x2f, 0x47, 0x2f, 0xe2, 0x22, 0xbd, 0x01, 0xe5, 0x25, 0xb6, 0x95,
		0xda, 0x3f, 0xf7, 0x03, 0xf4, 0x0e, 0xd6, 0x8c, 0xbb, 0x69, 0x1d, 0xcb, 0xd1, 0xe2, 0x60, 0xdb,
		0xf5, 0x0b, 0x85, 0x98, 0xe6, 0x17, 0xbe, 0x29, 0x4e, 0xa7, 0x90, 0x11, 0xac, 0xbc, 0xa5, 0x3e,
		0x05, 0xfe, 0xe9, 0x56, 0x93, 0x00, 0x00, 0x00, 0x01, 0x02
	};
#endif

	static const uint8_t
		DiffieHellmanGroupExchangeRequest = 34,
		DiffieHellmanGroupExchangeGroup = 31,
		DiffieHellmanGroupExchangeInitialize = 32,
		DiffieHellmanGroupExchangeReply = 33;

	/*
	 * XXX
	 * Like a non-trivial amount of other code, this has been
	 * written a bit fast-and-loose.  The usage of the dh_ and
	 * k_ in particularly are a bit dodgy and need to be freed
	 * in the destructor.
	 *
	 * Need to add assertions and frees.
	 */
	template<CryptoHash::Algorithm hash_algorithm>
	class DiffieHellmanGroupExchange : public SSH::KeyExchange {
		LogHandle log_;
		SSH::Session *session_;
		DH *dh_;
		Buffer key_exchange_;
		BIGNUM *k_;
	public:
		DiffieHellmanGroupExchange(SSH::Session *session, const std::string& key_exchange_name)
		: SSH::KeyExchange(key_exchange_name),
		  log_("/ssh/key_exchange/" + key_exchange_name),
		  session_(session),
		  dh_(NULL),
		  key_exchange_(),
		  k_()
		{ }

		~DiffieHellmanGroupExchange()
		{ }

		KeyExchange *clone(void) const
		{
			return (new DiffieHellmanGroupExchange(session_, name_));
		}

		bool hash(Buffer *out, const Buffer *in) const
		{
			return (CryptoHash::hash(hash_algorithm, out, in));
		}

		bool input(Filter* sender, Buffer *in)
		{
			SSH::ServerHostKey *key;
			uint32_t max, min, n;
			const BIGNUM *pr, *gr;
			const BIGNUM *er, *fr;
			BIGNUM *p, *g;
			BIGNUM *e, *f;
			Buffer server_public_key;
			Buffer signature;
			Buffer packet;
			Buffer group;
			Buffer exchange_hash;
			Buffer data;
			Buffer initialize;

			switch (in->peek()) {
			case DiffieHellmanGroupExchangeRequest:
				if (session_->role_ != SSH::ServerRole) {
					ERROR(log_) << "Received group exchange request as client.";
					return (false);
				}
				in->skip(1);
				key_exchange_ = *in;
				if (!SSH::UInt32::decode(&min, in))
					return (false);
				if (!SSH::UInt32::decode(&n, in))
					return (false);
				if (!SSH::UInt32::decode(&max, in))
					return (false);
				if (min < DH_GROUP_MIN)
					min = DH_GROUP_MIN;
				if (max > DH_GROUP_MAX)
					max = DH_GROUP_MAX;
				if (min > max)
					return (false);
				if (n < min)
					n = min;
				else if (n > max)
					n = max;

#ifdef USE_TEST_GROUP
				group.append(test_prime_and_generator, sizeof test_prime_and_generator);
				dh_ = DH_new();

				SSH::MPInt::decode(&p, &group);
				SSH::MPInt::decode(&g, &group);
#if OPENSSL_VERSION_NUMBER < 0x1010006fL
				dh_->p = p;
				dh_->g = g;
#else
				DH_set0_pqg(dh_, p, NULL, g);
#endif				ASSERT(log_, group.empty());
#else
				DEBUG(log_) << "Doing DH_generate_parameters for " << n << " bits.";
				ASSERT(log_, dh_ == NULL);
				dh_ = DH_generate_parameters(n, 2, NULL, NULL);
				if (dh_ == NULL) {
					ERROR(log_) << "DH_generate_parameters failed.";
					return (false);
				}
#endif

#ifdef USE_TEST_GROUP
				pr = p;
				gr = g;
#else
#if OPENSSL_VERSION_NUMBER < 0x1010006fL
				pr = dh_->p;
				gr = dh_->p;
#else
				DH_get0_pqg(dh_, &pr, NULL, &gr);
#endif
#endif

				SSH::MPInt::encode(&group, pr);
				SSH::MPInt::encode(&group, gr);
				key_exchange_.append(group);

				packet.append(DiffieHellmanGroupExchangeGroup);
				packet.append(group);
				sender->produce(packet);
				return (true);
			case DiffieHellmanGroupExchangeGroup:
				if (session_->role_ != SSH::ClientRole) {
					ERROR(log_) << "Received DH group as server.";
					return (false);
				}
				in->skip(1);
				key_exchange_.append(in);

				dh_ = DH_new();
				if (dh_ == NULL) {
					ERROR(log_) << "DH_new failed.";
					return (false);
				}

				if (!SSH::MPInt::decode(&p, in))
					return (false);
				if (!SSH::MPInt::decode(&g, in)) {
					BN_free(p);
					return (false);
				}

#if OPENSSL_VERSION_NUMBER < 0x1010006fL
				dh_->p = p;
				dh_->g = g;
#else
				DH_set0_pqg(dh_, p, NULL, g);
#endif
				if (!DH_generate_key(dh_)) {
					ERROR(log_) << "DH_generate_key failed.";
					return (false);
				}
#if OPENSSL_VERSION_NUMBER < 0x1010006fL
				er = dh_->pub_key;
#else
				DH_get0_key(dh_, &er, NULL);
#endif
				SSH::MPInt::encode(&initialize, er);
				key_exchange_.append(initialize);

				packet.append(DiffieHellmanGroupExchangeInitialize);
				packet.append(initialize);
				sender->produce(packet);
				return (true);
			case DiffieHellmanGroupExchangeInitialize:
				if (session_->role_ != SSH::ServerRole) {
					ERROR(log_) << "Received group exchange initialization as client.";
					return (false);
				}
				in->skip(1);
				key_exchange_.append(in);
				if (!SSH::MPInt::decode(&e, in))
					return (false);

				if (!DH_generate_key(dh_))
					return (false);
#if OPENSSL_VERSION_NUMBER < 0x1010006fL
				fr = dh_->pub_key;
#else
				DH_get0_key(dh_, &fr, NULL);
#endif
				SSH::MPInt::encode(&key_exchange_, fr);
				if (!exchange_finish(e)) {
					ERROR(log_) << "Server key exchange finish failed.";
					return (false);
				}

				key = session_->chosen_algorithms_.server_host_key_;
				if (!key->sign(&signature, &session_->exchange_hash_))
					return (false);
				key->encode_public_key(&server_public_key);

				packet.append(DiffieHellmanGroupExchangeReply);
				SSH::String::encode(&packet, server_public_key);
				SSH::MPInt::encode(&packet, f);
				SSH::String::encode(&packet, &signature);
				sender->produce(packet);

				sender->flush(SSH::ALGORITHM_NEGOTIATED);
				/*
				 * XXX
				 * Should send NEWKEYS.
				 */
				return (true);
			case DiffieHellmanGroupExchangeReply:
				if (session_->role_ != SSH::ClientRole) {
					ERROR(log_) << "Received group exchange reply as client.";
					return (false);
				}
				in->skip(1);
				if (!SSH::String::decode(&server_public_key, in))
					return (false);
				if (!SSH::MPInt::decode(&f, in))
					return (false);
				if (!SSH::String::decode(&signature, in))
					return (false);

				key = session_->chosen_algorithms_.server_host_key_;
				if (!key->decode_public_key(&server_public_key)) {
					ERROR(log_) << "Could not decode server public key:" << std::endl << server_public_key.hexdump();
					return (false);
				}

				SSH::MPInt::encode(&key_exchange_, f);
				if (!exchange_finish(f)) {
					ERROR(log_) << "Client key exchange finish failed.";
					return (false);
				}

				if (!key->verify(&signature, &session_->exchange_hash_)) {
					ERROR(log_) << "Failed to verify exchange hash.";
					return (false);
				}

				sender->flush(SSH::ALGORITHM_NEGOTIATED);
				/*
				 * XXX
				 * Should send NEWKEYS, but we're not ready for that yet.
				 * For now we just assume the peer will do it.  How lazy,
				 * no?
				 */
				return (true);
			default:
				ERROR(log_) << "Not yet implemented.";
				return (false);
			}
		}

		bool init(Buffer *out)
		{
			ASSERT(log_, out->empty());
			ASSERT(log_, session_->role_ == SSH::ClientRole);

			Buffer request;
			SSH::UInt32::encode(&request, DH_GROUP_MIN);
			SSH::UInt32::encode(&request, DH_GROUP_MIN);
			SSH::UInt32::encode(&request, DH_GROUP_MAX);

			key_exchange_ = request;

			out->append(DiffieHellmanGroupExchangeRequest);
			out->append(request);

			return (true);
		}

	private:
		bool exchange_finish(BIGNUM *remote_pubkey)
		{
			SSH::ServerHostKey *key;
			Buffer server_public_key;
			Buffer exchange_hash;
			Buffer data;

			ASSERT(log_, dh_ != NULL);

			uint8_t secret[DH_size(dh_)];
			int secretlen = DH_compute_key(secret, remote_pubkey, dh_);
			if (secretlen == -1)
				return (false);
			k_ = BN_bin2bn(secret, secretlen, NULL);
			if (k_ == NULL)
				return (false);

			key = session_->chosen_algorithms_.server_host_key_;
			key->encode_public_key(&server_public_key);

			SSH::String::encode(&data, session_->client_version_);
			SSH::String::encode(&data, session_->server_version_);
			SSH::String::encode(&data, session_->client_kexinit_);
			SSH::String::encode(&data, session_->server_kexinit_);
			SSH::String::encode(&data, server_public_key);
			data.append(key_exchange_);
			SSH::MPInt::encode(&data, k_);

			if (!CryptoHash::hash(hash_algorithm, &exchange_hash, &data))
				return (false);

			session_->exchange_hash_ = exchange_hash;
			SSH::MPInt::encode(&session_->shared_secret_, k_);
			if (session_->session_id_.empty())
				session_->session_id_ = exchange_hash;

			return (true);
		}
	};
}

void
SSH::KeyExchange::add_algorithms(SSH::Session *session)
{
	session->algorithm_negotiation_->add_algorithm(new DiffieHellmanGroupExchange<CryptoHash::SHA256>(session, "diffie-hellman-group-exchange-sha256"));
	session->algorithm_negotiation_->add_algorithm(new DiffieHellmanGroupExchange<CryptoHash::SHA1>(session, "diffie-hellman-group-exchange-sha1"));
}
