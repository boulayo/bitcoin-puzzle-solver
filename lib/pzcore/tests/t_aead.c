/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_aead.c - ChaCha20, Poly1305 and ChaCha20-Poly1305: the vectors of RFC 8439 (2.4.2, 2.5.2, 2.8.2 and the Poly1305 edge cases of
 * appendix A.3), and AEAD vectors at lengths around the block edges computed with the cryptography package of Python. */
#include <pzcore/aead.h>
#include "testutil.h"

static const char SUNSCREEN[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

static void unhex(const char *h, uint8_t *out, size_t *n)
{
    const int r = tu_unhex(h, out, 512);
    CHECK(r >= 0);
    *n = r < 0 ? 0 : (size_t)r;
}

static void test_chacha20(void)
{
    uint8_t key[32], nonce[12], out[128], want[128];
    size_t n;
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    unhex("000000000000004a00000000", nonce, &n);
    const size_t len = sizeof SUNSCREEN - 1;
    pz_chacha20_xor(key, nonce, 1, (const uint8_t *)SUNSCREEN, out, len);
    unhex("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d", want, &n);
    CHECK(n == len && memcmp(out, want, len) == 0);
    pz_chacha20_xor(key, nonce, 1, out, out, len);              /* in place, back to the text */
    CHECK(memcmp(out, SUNSCREEN, len) == 0);
}

static const struct { const char *key, *msg, *tag; } POLY[] = {
    {"85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", "43727970746f6772617068696320466f72756d2052657365617263682047726f7570", "a8061dc1305136c6c22b8baf0c0127a9"},
    {"0200000000000000000000000000000000000000000000000000000000000000", "ffffffffffffffffffffffffffffffff", "03000000000000000000000000000000"},
    {"02000000000000000000000000000000ffffffffffffffffffffffffffffffff", "02000000000000000000000000000000", "03000000000000000000000000000000"},
    {"0100000000000000000000000000000000000000000000000000000000000000", "fffffffffffffffffffffffffffffffff0ffffffffffffffffffffffffffffff11000000000000000000000000000000", "05000000000000000000000000000000"},
    {"0100000000000000000000000000000000000000000000000000000000000000", "fffffffffffffffffffffffffffffffffbfefefefefefefefefefefefefefefe01010101010101010101010101010101", "00000000000000000000000000000000"},
    {"0200000000000000000000000000000000000000000000000000000000000000", "fdffffffffffffffffffffffffffffff", "faffffffffffffffffffffffffffffff"},
    {"0100000000000000040000000000000000000000000000000000000000000000", "e33594d7505e43b900000000000000003394d7505e4379cd01000000000000000000000000000000000000000000000001000000000000000000000000000000", "14000000000000005500000000000000"},
    {"0100000000000000040000000000000000000000000000000000000000000000", "e33594d7505e43b900000000000000003394d7505e4379cd010000000000000000000000000000000000000000000000", "13000000000000000000000000000000"},
};

static void test_poly1305(void)
{
    for (size_t i = 0; i < sizeof POLY / sizeof *POLY; i++) {
        uint8_t key[32], msg[512], want[16], tag[16];
        size_t kn, mn, tn;
        unhex(POLY[i].key, key, &kn), unhex(POLY[i].msg, msg, &mn), unhex(POLY[i].tag, want, &tn);
        pz_poly1305(key, msg, mn, tag);
        if (memcmp(tag, want, 16) != 0) printf("  poly1305 vector %zu\n", i);
        CHECK(memcmp(tag, want, 16) == 0);
    }
}

static const struct { const char *key, *nonce, *ad, *pt, *ct, *tag; } AEAD[] = {
    {"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", "070000004041424344454647", "50515253c0c1c2c3c4c5c6c7", NULL,
     "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116",
     "1ae10b594f09e26a7e902ecbd0600691"},
    {"6a6d5e7c7c73af81d2de7793083eaba2e763d0d31f4d190fb1c8974842284bbe", "f423eabeb189d232c739c215", "",
     "",
     "",
     "353eb0fe440ee8ae2341414c673b03f8"},
    {"4cde9f2bc1d8193c87d87e61fd19c283414a07471068a06b89b2bf70e08c50da", "d91c316b5e273a5a2be8fd27", "",
     "37",
     "b9",
     "b28857066145a8c9044b7bd5987d069f"},
    {"3d9e53aa9d115712c69cee8ae2bc9905c9849a803c3dad6ad3c13bf1a6c73dd9", "e13457f5ff4d60b5284c6849", "e16e47",
     "0c7986beb91a2b67e4a8fbb30770bc",
     "609e57c8707104dc4ee0a7f1cfb7c0",
     "ec9575400a1ce4ecec32587214b3b235"},
    {"a4b318a6bc17173fae38cc59a5b1d1957632e87c59e8015e4601c18838467783", "8e03fc0e7b6f9ab9d8cd170b", "a8206b39496a970f4b5491dd60ea1a1a",
     "811907d2d7ca2b47bb7964109af8c862",
     "e7e60bccae8352717e86d0c4698e756a",
     "7174b69e0aab12826b71bc8e1a17bbf6"},
    {"2ff85375391b7b65a9123fe7736899013d5a84a7d23de9bd89f0249c67ec0481", "a45adbb57c07ad1722554354", "e9",
     "f8ba0b6bc1758e7bd73e4d342413f80a5d",
     "0ef5a729f6be3a62c94886b408fdb59f77",
     "a65bad4e063d1878b1e7851921228cc3"},
    {"4e0a74cf58a88ac3df9f6047172217e81e62808d44b4926778322bdc1f210863", "8c434b4f7fbb5ebc9176a784", "c107b3acdf13bac81005f81f",
     "61db7fc87a42acc9e381296d8bf2c54c93879f0fa60f19560170d487e9cee1f56dad600c250e50278846bcbd353163341193899dd501bb5e9752f2a9194a8c",
     "2012189797ddbfc9940bb1e8e98bb11202851ac54746459a456f167cce16dca8130fadad511a44232951679174c3f76483b8cbdbd30c6e4e47f2cf7d544cc2",
     "9ede6a1e66eb71534efbd65760c2ef5c"},
    {"5779eeb4ab996148b142eaa0c88a47844165697f2731027138b257472a2349a4", "986e4e94cf62840e164d3e0d", "",
     "6a96945062c1524e3a59c2d34c6ddd646d445e5beaf81187c8dd2bf7ed5ef274d9eba3442419924ea215dc04aa5cbdf6e429c87ce3d50a22c90f065241bcfd09",
     "9b6e7e5edb95b6318c82df643640d6f0333067a22482e7a99cd79c5a09439bfbd9c27c9c48752b8d787a24017458610d2f367f3c0add2a4460effab6a869c18b",
     "0d8a2da901a381e7c3d5b0d7f0f32324"},
    {"006a2ced6800dd2179b08f57ad69ae09eda7abcc2d78b1bbbd3237df2bd0e45d", "c71eacd34e34d78320a80c03", "c97a86473602c89e300bba05621c251abaf19ae310c723d3575be105b8eae43646",
     "e654175f33ca5de9cc0f125561ae4c78940d097b6311f3f7aab684940fa7c8cbfd780d4f7e637edfe4d68ebf4ace4ad520ef06f4bbb07b71aafe3e9ead9c4c94e8",
     "892a5fff696306fecad2984716e73c1d0ee78568550a77bce0d0ba47f4052f7af6ecc2a7469726355a0beaae2bace7a9799f7d9a0c88fab14fe5112c94adcda87f",
     "9e1ef3988c1620aef6337ee1d33eb839"},
    {"afe72ee2483f21797ae8895cbfe8970b9b184db85eb66e1950c611eb2e588193", "9a5840a22ec97d4cfa1a68f9", "2cfde15d28028c",
     "e22b26167b81c61965e2db42d9ffc7d8fb637d80ef3c97b4eb768810d9b0fcedb61cec51f38248fffa9467b89a6e9642888dd43b0a486d865d517ee4597029f14363fbc6ea544adc8a34656a2cc25cb8c574d6164bd66dbaca827e8a20b1fec6b1a7cb0aa5e6a4275da72b41e2ccab476abec1b4afc50b87ec5e376cb3a1f33ba7",
     "fb064b4ab790eae4eef702897600db9592c9920ca616afe8fbe8317c6dc6cee2b12123db1e81e52da2ad0cb4d6df8a875ad26302405e3b90edb9af8dc9df723b0e6860169e624dd9e438c24526916b970ea16d709bfe3d9e2697d38940bd34bd93d8bcbd8d3337c7292fea8b92e6f4b9e32650c18152b730b0757886585f6f173f",
     "c457998681b1012d767cb22b099ec6ef"},
    {"f4e5c333c6949e2725a79716a57cc8082b7167582f72bda0c6f8c936811b587f", "343af000bf0e325b8fb5c44e", "5342f71dddfd04b8488fe1533b9c9d2b67d57414ade9df6bceda5d463eed90b0ca87f6e10c1a6b92",
     "25b49802b1234bd36497e4504bb4a5cee2a2b7de2b576461da2af99733680776dbb4c89e0f6819359793cb106ebed3c2b38d3c3e3e181ffdd64e156770ab52856828c01f7c71cfe4fa67ec74bfd4c472eaa869eb7e9d4b11c1e83c5d8d381adcff79b0f7fde82b8dc828bc00fb2fc70b8039c3403f0c47038c95471ea8dc2aa8bc5ce9c0d330189be760101813f54ccb650c16b1c43517ded056c007533c85732b00e5a637d5f9d4c1d4f098764a9816bfd8772e55bdfb707c0bda5d25354e0b66cdfa7cfb2d08ffae035fb17dd6498065dbfe84cee336c9a9dc4b191973ca60f90bc45fe972246c9ea364e86b423986cc4c01239b5f9d734f035d1c5a814ba22a8a17600030ad01dad0f32a59b8a23e14915d99b42082c68cdfbc63abd3fba9dd32aeb337130fdee3b10b9b",
     "c166adc55fc2d8d15c068da21958e62df2eda375a8053b819d09b3bb982cdf26f5655986324d18d648db39405e200aee2cc4c687ee583b9ae7b8bee463c30f87b3d970a710e7cc7bd0008ccab1dc386b37b47519d4e35bbd0f1f55379bf20d8b27be7bce4890a26b2bd783c77da718b343680b4ca5156a656998f131ab19d19d3c04cbbfecf80e93059b5fee9a61d3bbdd747a514de047274dd19009b5706d5a811fd99a1f8495e972bd417d9931f2835192d997a071902fe940476a7666616103f8500c8dc446949aa1e37a10087e598cd20aea69e64844bfa0e05b294c9b6e05a0cc73d5b7ca0569a086fc9d5af6157e5047d840484a11bef0423a1b97034ad7b652cf1ee1dd37f75b8ee18ed36bbc616ea9c1b733faffd77db5c81aba3a34cf29d4d5d0724c19c2509a11",
     "3be3d4e6edfdcec8806ff5b80e8c2332"},
};

static void test_aead(void)
{
    for (size_t i = 0; i < sizeof AEAD / sizeof *AEAD; i++) {
        uint8_t key[32], nonce[12], ad[64], pt[512], ct[512], want[512], wtag[16], tag[16], back[512];
        size_t kn, nn, an, pn, cn, tn;
        unhex(AEAD[i].key, key, &kn), unhex(AEAD[i].nonce, nonce, &nn), unhex(AEAD[i].ad, ad, &an);
        if (AEAD[i].pt) unhex(AEAD[i].pt, pt, &pn);
        else memcpy(pt, SUNSCREEN, pn = sizeof SUNSCREEN - 1);
        unhex(AEAD[i].ct, want, &cn), unhex(AEAD[i].tag, wtag, &tn);
        CHECK(cn == pn);
        pz_aead_seal(key, nonce, ad, an, pt, pn, ct, tag);
        if (memcmp(ct, want, pn) != 0 || memcmp(tag, wtag, 16) != 0) printf("  aead vector %zu (length %zu)\n", i, pn);
        CHECK(memcmp(ct, want, pn) == 0 && memcmp(tag, wtag, 16) == 0);
        memset(back, 0xee, sizeof back);
        CHECK(pz_aead_open(key, nonce, ad, an, ct, pn, tag, back) && memcmp(back, pt, pn) == 0);
        /* a changed byte of the text, of the data or of the tag is refused, and the output is left as it was */
        if (pn) {
            ct[pn / 2] ^= 1;
            memset(back, 0xee, sizeof back);
            CHECK(!pz_aead_open(key, nonce, ad, an, ct, pn, tag, back) && back[0] == 0xee);
            ct[pn / 2] ^= 1;
        }
        if (an) {
            ad[0] ^= 0x80;
            CHECK(!pz_aead_open(key, nonce, ad, an, ct, pn, tag, back));
            ad[0] ^= 0x80;
        }
        tag[15] ^= 1;
        CHECK(!pz_aead_open(key, nonce, ad, an, ct, pn, tag, back));
        tag[15] ^= 1;
        CHECK(pz_aead_open(key, nonce, ad, an, ct, pn, tag, ct) && memcmp(ct, pt, pn) == 0);   /* in place */
    }
}

int main(void)
{
    test_chacha20();
    test_poly1305();
    test_aead();
    TEST_DONE("t_aead");
}
