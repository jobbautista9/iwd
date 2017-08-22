import socket
import os
import threading
from Crypto.Cipher import AES

class AuthCenter:
    '''
        Home Location Register (HLR) and Authentication Center (AuC) used for
        for retrieving SIM/AKA values. This provides a UDP server that
        hostapd can communicate with to obtain SIM values.
    '''
    def __init__(self, sock_path, config_file):
        self._read_config(config_file)
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._socket.setblocking(0)
        if os.path.isfile(sock_path):
            os.unlink(sock_path)
        self._socket.bind(sock_path)

        self._rxhandle = threading.Thread(target=self._rx_thread)
        self._rxhandle.shutdown = False
        self._rxhandle.start()

    def _rx_thread(self):
        while (True):
            if self._rxhandle.shutdown == True:
                break
            try:
                data, addr = self._socket.recvfrom(1000)
                data = data.decode('ascii')
                resp = self._process_data(data)
            except:
                continue
            if resp:
                self._socket.sendto(bytearray(resp, 'UTF-8'), addr)

    def _read_config(self, file):
        self._database = {}
        with open(file) as f:
            for line in f:
                if line[0] == '#':
                    continue
                else:
                    data = line.split(':')
                    self._database[data[0]] = ':'.join(data[1:])

    def _process_data(self, data):
        if data[:12] == "SIM-REQ-AUTH":
            # SIM requests just return the stored values for the IMSI
            imsi, num_chals = data[13:].split(' ')
            data = self._database[imsi]

            response = "SIM-RESP-AUTH %s" % imsi
            response += (' ' + data)*int(num_chals)

            return response
        elif data[:12] == "AKA-REQ-AUTH":
            # AKA requests must compute the milenage parameters for the IMSI
            imsi = data.split(' ')[1]
            data = self._database[imsi]

            k, opc, amf, sqn = data.split(':')

            rand = self._bytetostring(os.urandom(16))

            response = "AKA-RESP-AUTH %s " % imsi

            return response + self._get_milenage(opc, k, rand, sqn, amf)

    def _bytetostring(self, b):
        return ''.join(format(x, '02x') for x in b)

    def _xor(self, a, b):
        ret = bytearray(16)
        for i in range(len(a)):
            ret[i] = a[i] ^ b[i]
        return ret

    def _get_milenage(self, opc, k, rand, sqn, amf):
        '''
            Computes milenage values from OPc, K, RAND, SQN and AMF
            Returns a concatenated list (RAND + AUTN + IK + CK + RES) that
            will be sent back as the response to the client (hostapd). This
            is a python re-write of the function eap_aka_get_milenage() from
            src/simutil.c
        '''
        opc = bytearray.fromhex(opc)
        k = bytearray.fromhex(k)
        # rand gets returned, so it should be left as a hex string
        _rand = bytearray.fromhex(rand)
        sqn = bytearray.fromhex(sqn)
        amf = bytearray.fromhex(amf)

        aes1 = AES.new(bytes(k), AES.MODE_ECB)
        tmp1 = self._xor(_rand, opc)
        tmp1 = aes1.encrypt(bytes(tmp1))
        tmp1 = bytearray(tmp1)

        tmp2 = bytearray()
        tmp2[0:6] = sqn
        tmp2[6:2] = amf
        tmp2[9:6] = sqn
        tmp2[15:2] = amf

        tmp3 = bytearray(16)
        for i in range(len(tmp1)):
            tmp3[(i + 8) % 16] = tmp2[i] ^ opc[i]

        tmp3 = self._xor(tmp3, tmp1)

        aes2 = AES.new(bytes(k), AES.MODE_ECB)
        tmp1 = aes2.encrypt(bytes(tmp3))
        tmp1 = bytearray(tmp1)

        tmp1 = self._xor(tmp1, opc)
        maca = self._bytetostring(tmp1)

        tmp1 = self._xor(_rand, opc)
        aes3 = AES.new(bytes(k), AES.MODE_ECB)
        tmp2 = aes3.encrypt(bytes(tmp1))
        tmp2 = bytearray(tmp2)

        tmp1 = self._xor(tmp2, opc)
        tmp1[15] ^= 1

        aes4 = AES.new(bytes(k), AES.MODE_ECB)
        tmp3 = aes4.encrypt(bytes(tmp1))
        tmp3 = bytearray(tmp3)

        tmp3 = self._xor(tmp3, opc)

        res = self._bytetostring(tmp3[8:16])
        ak = self._bytetostring(tmp3[0:6])

        for i in range(len(tmp1)):
            tmp1[(i + 12) % 16] = tmp2[i] ^ opc[i]

        tmp1[15] ^= 1 << 1
        aes5 = AES.new(bytes(k), AES.MODE_ECB)
        tmp1 = aes5.encrypt(bytes(tmp1))
        tmp1 = bytearray(tmp1)

        tmp1 = self._xor(tmp1, opc)
        ck = self._bytetostring(tmp1)

        for i in range(len(tmp1)):
            tmp1[(i + 8) % 16] = tmp2[i] ^ opc[i]

        tmp1[15] ^= 1 << 2
        aes6 = AES.new(bytes(k), AES.MODE_ECB)
        tmp1 = aes6.encrypt(bytes(tmp1))
        tmp1 = bytearray(tmp1)
        tmp1 = self._xor(tmp1, opc)
        ik = self._bytetostring(tmp1)

        tmp1 = bytearray.fromhex(ak)
        autn = bytearray(6)
        for i in range(0, 6):
            autn[i] = sqn[i] ^ tmp1[i]

        autn[6:2] = amf
        autn[8:8] = bytearray.fromhex(maca)[0:8]

        autn = self._bytetostring(autn)

        return rand + ' ' + autn + ' ' + ik + ' ' + ck + ' ' + res

    def stop(self):
        '''
            Stop the Authentication server and close the socket
        '''
        self._rxhandle.shutdown = True
        self._rxhandle.join()
        self._socket.close()