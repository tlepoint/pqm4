#include "core.h"

void xor_bytes(uint8_t *r, const uint8_t *f, const uint8_t *g, int len) {

	int i;
	for (i = 0; i < len; i++)
		r[i] = f[i] ^ g[i];
}

void recover_s1(uint8_t *s1, poly_d v) {
#if KINDI_KEM_S1BITS == 1
	int i, a, pos = 0;

	memset(s1, 0, KINDI_KEM_S1SIZE);
	for (i = 0; i < KINDI_KEM_S1SIZE; i++) {
		for (a = 0; a < 8; a++) {

			  s1[i] |= (lround((float)v[pos++]/(KINDI_KEM_Q/2)) & 1) << a;
		}
	}
#elif KINDI_KEM_S1BITS == 2
	int i, a, pos = 0;

	memset(s1, 0, KINDI_KEM_S1SIZE);
	for (i = 0; i < KINDI_KEM_S1SIZE; i++) {
		for (a = 0; a < 4; a++) {

			s1[i] |= (lround((float)v[pos++]/(KINDI_KEM_Q/4)) & 0x03) << (2*a);
		}
	}


#endif

}

void kindi_keygen(kindi_pk *pk, poly_d *sk_r) {

	int i, x;
	poly_d **A, *e, tmp;

	A = (poly_d**) memalign(32, KINDI_KEM_L * sizeof(poly_d*));
	e = (poly_d*) memalign(32, KINDI_KEM_L * sizeof(poly_d));
	for (i = 0; i < KINDI_KEM_L; i++)
		A[i] = (poly_d*) memalign(32, KINDI_KEM_L * sizeof(poly_d));

	// \mu <- {0,1}^n
	randombytes(pk->seed, KINDI_KEM_SEEDSIZE);

	// A \in R_q^{lxl} <- gen_matrix(\mu)
	poly_gen_matrix(A, pk->seed);

	// sample r,e with coefficients in [-r_sec,r_sec)
	uint8_t *gamma = malloc(KINDI_KEM_SEEDSIZE);
	randombytes(gamma,KINDI_KEM_SEEDSIZE);
	poly_setrandom_rsec(sk_r,e,gamma);
	free(gamma);

	//cache FFT of sk to avoid redundant FFT calculations in poly_mul
	poly_f *r_real, *r_imag;
	r_real = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));
	r_imag = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));
	for (i = 0; i < KINDI_KEM_L; i++)
		psi_loop_and_fft(sk_r[i], r_real[i], r_imag[i]);

	// b = A*r + e
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_init(pk->b[i]);
		for (x = 0; x < KINDI_KEM_L; x++) {

			poly_mul_precomp_fft(tmp, A[i][x], r_real[x], r_imag[x]);
			poly_add_nored(pk->b[i], pk->b[i], tmp);
		}
		poly_add_nored(pk->b[i], pk->b[i], e[i]);
		poly_coeffreduce_pos(pk->b[i]);
	}

	for (i = 0; i < KINDI_KEM_L; i++)
		free(A[i]);
	free(A);
	free(r_real);
	free(r_imag);
	free(e);

}

void kindi_kem_encrypt(kindi_pk *pk, uint8_t *d, uint8_t *s1, poly_d *cipher) {

	int i, x;

	poly_d *s, **A, *u_encoded, tmp;
	s = (poly_d *) memalign(32, KINDI_KEM_L * sizeof(poly_d));
	u_encoded = (poly_d*) memalign(32, (KINDI_KEM_L + 1) * sizeof(poly_d));
	A = (poly_d**) memalign(32, KINDI_KEM_L * sizeof(poly_d*));
	for (i = 0; i < KINDI_KEM_L; i++)
		A[i] = (poly_d*) memalign(32, KINDI_KEM_L * sizeof(poly_d));

	poly_f *s_real, *s_imag;
	s_real = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));
	s_imag = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));

	uint8_t *u, *message_padded;
	u = malloc(KINDI_KEM_MESSAGEBYTES);
	message_padded = calloc(KINDI_KEM_MESSAGEBYTES, 1);

	//padding with 0
	memcpy(message_padded, d, KINDI_KEM_HASHSIZE);

	// u_bar,s_1_bar,(s_2,...,s_l) <- G(s1)
	gen_randomness(s, u, s1);

	// u = u_bar XOR msg
	xor_bytes(u, u, message_padded, KINDI_KEM_HASHSIZE);

	// u_encoded = e = Encode(u) - r_sec;
	for (x = 0; x < KINDI_KEM_L + 1; x++) {
		poly_frombytes_bitlen(u_encoded[x],
				u + x * (KINDI_KEM_N * (KINDI_KEM_LOG2RSEC + 1) / 8),
				KINDI_KEM_LOG2RSEC + 1);
		for (i = 0; i < KINDI_KEM_N; i++) {
			u_encoded[x][i] -= KINDI_KEM_RSEC;
		}
	}

	//cache FFT of s_i to avoid redundant FFT calculations in poly_mul
	for (i = 0; i < KINDI_KEM_L; i++)
		psi_loop_and_fft(s[i], s_real[i], s_imag[i]);

	// A <- gen_matrix(\mu)
	poly_gen_matrix(A, pk->seed);

	// (c_1,c_2,...,c_L)
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_init(cipher[i]);
		for (x = 0; x < KINDI_KEM_L; x++) {
			// A^t * s
			poly_mul_precomp_fft(tmp, A[x][i], s_real[x], s_imag[x]);
			poly_add_nored(cipher[i], cipher[i], tmp);
		}

		// A^t*s + e
		poly_add_nored(cipher[i], cipher[i], u_encoded[i]);
		poly_coeffreduce_pos(cipher[i]);
	}

	// c_{L2+1} = (b_1-g_{k-1},b_2,...,b_L)*s + e_{L+1}
#if KINDI_KEM_S1BITS == 1
	pk->b[0][0] += (1 << (KINDI_KEM_LOGQ - 1));
#elif KINDI_KEM_S1BITS == 2
	pk->b[0][0] += (1 << (KINDI_KEM_LOGQ - 2));
#endif
	poly_init(cipher[KINDI_KEM_L]);
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_mul_precomp_fft(tmp, pk->b[i], s_real[i], s_imag[i]);
		poly_add_nored(cipher[KINDI_KEM_L], cipher[KINDI_KEM_L], tmp);
	}
#if  KINDI_KEM_S1BITS == 2
	poly_sub_constant(cipher[KINDI_KEM_L], cipher[KINDI_KEM_L],(1 << (KINDI_KEM_LOGQ-2))*KINDI_KEM_RSEC);
#endif
	poly_add_nored(cipher[KINDI_KEM_L], cipher[KINDI_KEM_L],
			u_encoded[KINDI_KEM_L]);
	poly_coeffreduce_pos(cipher[KINDI_KEM_L]);

	free(s);
	for (i = 0; i < KINDI_KEM_L; i++)
		free(A[i]);
	free(A);
	free(u);
	free(u_encoded);
	free(s_real);
	free(s_imag);
	free(message_padded);
}

void kindi_kem_decrypt(poly_d *sk_r, kindi_pk *pk, poly_d *cipher,
		uint8_t *d_rec, uint8_t *s1_rec) {

	int i, x;

	poly_d *u_rec, *s_rec, **A, v, tmp;
	u_rec = (poly_d *) memalign(32, (KINDI_KEM_L + 1) * sizeof(poly_d));
	s_rec = (poly_d *) memalign(32, KINDI_KEM_L * sizeof(poly_d));
	A = (poly_d**) memalign(32, KINDI_KEM_L * sizeof(poly_d *));
	for (i = 0; i < KINDI_KEM_L; i++)
		A[i] = (poly_d*) memalign(32, KINDI_KEM_L * sizeof(poly_d));

	poly_f *s_rec_real, *s_rec_imag;
	s_rec_real = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));
	s_rec_imag = (poly_f*) memalign(32, KINDI_KEM_L * sizeof(poly_f));

	uint8_t *u_bar, *u_rec_bytes;
	u_bar = malloc(KINDI_KEM_MESSAGEBYTES);
	u_rec_bytes = malloc(KINDI_KEM_MESSAGEBYTES);

	// A <- SampleU(\mu)
	poly_gen_matrix(A, pk->seed);

	// v = c_{L+1} - (c1,...,c_L)*r = g_{k-1}*s + small
	poly_init(v);
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_mul(tmp, cipher[i], sk_r[i]);
		poly_add_nored(v, v, tmp);
	}
	poly_sub_nored(v, cipher[KINDI_KEM_L], v);
	poly_coeffreduce_pos(v);

	// s1 = Recover(v)
	recover_s1(s1_rec, v);

	// u_bar,s_1_bar,(s_2,...,s_l) <- G(s1)
	gen_randomness(s_rec, u_bar, s1_rec);

	//cache FFT of s_i to avoid redundant FFT calculations in poly_mul
	for (i = 0; i < KINDI_KEM_L; i++)
		psi_loop_and_fft(s_rec[i], s_rec_real[i], s_rec_imag[i]);

	// (u_1,u_2,...,u_L) = cipher - A^t * s
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_init(u_rec[i]);
		for (x = 0; x < KINDI_KEM_L; x++) {
			//A^t * s
			poly_mul_precomp_fft(tmp, A[x][i], s_rec_real[x], s_rec_imag[x]);
			poly_add_nored(u_rec[i], u_rec[i], tmp);
		}
		poly_sub_nored(u_rec[i], cipher[i], u_rec[i]);
		poly_coeffreduce_center(u_rec[i]);
	}

	// u_{L+1} = cipher_{L+1} - b * s
#if  KINDI_KEM_S1BITS == 1
	pk->b[0][0] += (1 << (KINDI_KEM_LOGQ - 1));
#elif  KINDI_KEM_S1BITS == 2
	pk->b[0][0] += (1 << (KINDI_KEM_LOGQ - 2));
#endif
	poly_init(u_rec[KINDI_KEM_L]);
	for (i = 0; i < KINDI_KEM_L; i++) {
		poly_mul_precomp_fft(tmp, pk->b[i], s_rec_real[i], s_rec_imag[i]);
		poly_add_nored(u_rec[KINDI_KEM_L], u_rec[KINDI_KEM_L], tmp);
	}

	poly_sub_nored(u_rec[KINDI_KEM_L], cipher[KINDI_KEM_L], u_rec[KINDI_KEM_L]);
	poly_coeffreduce_center(u_rec[KINDI_KEM_L]);

	// Decode(u)
	for (x = 0; x < KINDI_KEM_L + 1; x++) {

		for (i = 0; i < KINDI_KEM_N; i++) {
			u_rec[x][i] += KINDI_KEM_RSEC;
		}
		poly_tobytes_bitlen(
				u_rec_bytes + x * (KINDI_KEM_N * (KINDI_KEM_LOG2RSEC + 1) / 8),
				u_rec[x],
				KINDI_KEM_LOG2RSEC + 1);
	}

	// msg = d = Decode(u) XOR u_bar
	xor_bytes(d_rec, u_rec_bytes, u_bar, KINDI_KEM_MESSAGEBYTES);

	free(s_rec);
	free(u_bar);
	free(u_rec);
	free(u_rec_bytes);
	for (i = 0; i < KINDI_KEM_L; i++)
		free(A[i]);
	free(A);
	free(s_rec_real);
	free(s_rec_imag);

}

void kindi_kem_encaps(kindi_pk *pk_p, uint8_t *ct, uint8_t *ss) {

	int i;
	uint8_t *s1 = malloc(KINDI_KEM_S1SIZE);

	uint8_t *d = malloc(KINDI_KEM_HASHSIZE);

	poly_d *cipher = (poly_d *) memalign(32,
	KINDI_KEM_NUMBER_CIPHERPOLY * sizeof(poly_d));

	// s1 <- {0,1}^{KINDI_KEM_S1SIZE*8}
	randombytes(s1, KINDI_KEM_S1SIZE);


	// d <- H(s1)
	kindi_crypto_stream_h(d, KINDI_KEM_HASHSIZE, s1, KINDI_KEM_S1SIZE);

	// cipher <- KINDI_CPA.Encrypt(pk,d;s1)
	kindi_kem_encrypt(pk_p, d, s1, cipher);

	// convert ciphertext to byte-array
	int offset_ct = 0;
	for (i = 0; i < KINDI_KEM_NUMBER_CIPHERPOLY; i++) {

		poly_tobytes_bitlen(ct + offset_ct, cipher[i], KINDI_KEM_LOGQ);
		offset_ct += KINDI_KEM_CIPHER_POLYBYTES;
	}

	int hash_inputlen = KINDI_KEM_CIPHERBYTES+ KINDI_KEM_S1SIZE;
	uint8_t *hash_in = malloc(hash_inputlen);
	memcpy(hash_in, ct, KINDI_KEM_CIPHERBYTES);
	memcpy(hash_in + KINDI_KEM_CIPHERBYTES, s1,
			KINDI_KEM_S1SIZE);
	//  K <- H'(s1,(\vec{c},c))
	kindi_crypto_stream_hprime(ss, KINDI_KEM_HASHSIZE, hash_in, hash_inputlen);
	free(hash_in);
	free(cipher);
	free(d);
	free(s1);

}

void kindi_kem_decaps(poly_d *sk_p, kindi_pk *pk_p, poly_d *cipher,
		const uint8_t *ct, uint8_t *ss) {

	uint8_t *d_rec, *s1_rec, *d;
	d_rec = malloc(KINDI_KEM_MESSAGEBYTES);
	d = malloc(KINDI_KEM_HASHSIZE);

	s1_rec = malloc(KINDI_KEM_S1SIZE);

	// (d',s1') <- KINDI_CPA.Decrypt(sk,(\vec{c},c))
	kindi_kem_decrypt(sk_p, pk_p, cipher, d_rec, s1_rec);

	// d <- H(s1)
	kindi_crypto_stream_h(d, KINDI_KEM_HASHSIZE, s1_rec, KINDI_KEM_S1SIZE);

	int hash_inlen = KINDI_KEM_CIPHERBYTES + KINDI_KEM_S1SIZE;
	uint8_t *hash_in = malloc(hash_inlen);

	memcpy(hash_in, ct, KINDI_KEM_CIPHERBYTES);
	uint8_t *zero = calloc(KINDI_KEM_MESSAGEBYTES, 1);

	// d' == d ? and rest of message == 0?
	if (memcmp(d, d_rec, KINDI_KEM_HASHSIZE) == 0
			&& memcmp(d_rec + KINDI_KEM_HASHSIZE, zero,
					KINDI_KEM_MESSAGEBYTES - KINDI_KEM_HASHSIZE) == 0) {

		// return H'(s1',(\vec{c},c))
		memcpy(hash_in + KINDI_KEM_CIPHERBYTES, s1_rec,
				KINDI_KEM_S1SIZE);
	}

	else {
		uint8_t *uniform_rand = malloc(KINDI_KEM_S1SIZE);
		randombytes(uniform_rand, KINDI_KEM_S1SIZE);

		// return H'(random,(\vec{c},c))
		memcpy(hash_in + KINDI_KEM_CIPHERBYTES,
				uniform_rand, KINDI_KEM_S1SIZE);
		free(uniform_rand);
	}

	kindi_crypto_stream_hprime(ss, KINDI_KEM_HASHSIZE, hash_in, hash_inlen);
	free(hash_in);
	free(zero);
	free(d_rec);
	free(s1_rec);
}

