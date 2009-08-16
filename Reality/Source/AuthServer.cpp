#include "Common.h"
#include "AuthServer.h"
#include "AuthSocket.h"
#include "Log.h"
#include "Config.h"

initialiseSingleton( AuthServer );

void AuthServer::GenerateRSAKeys( unsigned int keyLen,CryptoPP::RSA::PublicKey &publicOutput, CryptoPP::RSA::PrivateKey &privateOutput )
{
	CryptoPP::InvertibleRSAFunction params;
	params.GenerateRandomWithKeySize( randPool, keyLen );
	publicOutput = CryptoPP::RSA::PublicKey(params);
	privateOutput = CryptoPP::RSA::PrivateKey(params);
}

void AuthServer::GenerateSignKeys( string &privKeyOut, string &pubKeyOut )
{
	for (;;)
	{
		CryptoPP::RSA::PublicKey publicKey;
		CryptoPP::RSA::PrivateKey privateKey;
		GenerateRSAKeys(2048,publicKey,privateKey);

		//just dump private key to file so we can load it
		CryptoPP::StringSink privOutput(privKeyOut);
		privateKey.DEREncodePrivateKey(privOutput);
		privOutput.MessageEnd();

		//for public key, we will just dump exponent as modulus is always 17
		CryptoPP::StringSink pubOutput(pubKeyOut);
		publicKey.GetModulus().Encode(pubOutput,publicKey.GetModulus().MinEncodedSize());
		pubOutput.MessageEnd();

		//make sure its exactly 256 bytes, if not, regenerate
		if (pubKeyOut.size() == (2048/8))
		{
			break;
		}
		else
		{
			//clear the output strings and try again
			privKeyOut = string();
			pubKeyOut = string();
			continue;
		}
	}
}

void AuthServer::LoadSignKeys()
{
	ifstream f_signPriv;
	f_signPriv.open("signPriv.dat",ios::binary);
	if (f_signPriv.is_open())
	{
		f_signPriv.seekg(0,ios::end);
		ifstream::pos_type keySize = f_signPriv.tellg();
		vector<byte> storage;
		storage.resize(keySize);
		f_signPriv.seekg(0,ios::beg);
		f_signPriv.read((char*)&storage[0],storage.size());
		f_signPriv.close();

		string signPrivStr = string((const char*)&storage[0],storage.size());

		CryptoPP::InvertibleRSAFunction params;
		CryptoPP::StringSource pkeySource(signPrivStr,true);
		params.BERDecodePrivateKey(pkeySource,false,pkeySource.MaxRetrievable());

		CryptoPP::RSA::PrivateKey privateKey( params );
		CryptoPP::RSA::PublicKey publicKey( params );

		signer2048bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer(privateKey));
		verifier2048bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier(publicKey));
	}
	else
	{
		//generate new signing key
		string outPrivate, outPublic;
		GenerateSignKeys(outPrivate,outPublic);
		ofstream fileStream;
		if (outPrivate.size() > 0)
		{
			//initialize RSA engine
			CryptoPP::InvertibleRSAFunction params;
			CryptoPP::StringSource pkeySource(outPrivate,true);
			params.BERDecodePrivateKey(pkeySource,false,pkeySource.MaxRetrievable());
			CryptoPP::RSA::PrivateKey privateKey( params );
			CryptoPP::RSA::PublicKey publicKey( params );
			signer2048bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer(privateKey));
			verifier2048bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier(publicKey));	

			//write to file
			fileStream.open("signPriv.dat",ios::binary | ios::trunc);
			fileStream.write(outPrivate.data(),outPrivate.size());
			fileStream.close();
		}
		if (outPublic.size() > 0)
		{
			//write to file, we will copy this to mxo exe
			fileStream.open("signPub.dat",ios::binary | ios::trunc);
			fileStream.write(outPublic.data(),outPublic.size());
			fileStream.close();
		}
	}
}

ByteBuffer AuthServer::MessageFromPublicKey( CryptoPP::RSA::PublicKey &inputKey )
{
	//message to be signed/verified is just binary dump of modulus (128 bytes) and then binary dump of exponent (1 byte)
	vector<byte> modulusBinary;
	modulusBinary.resize(inputKey.GetModulus().MinEncodedSize());
	vector<byte> exponentBinary;
	exponentBinary.resize(inputKey.GetPublicExponent().MinEncodedSize());
	inputKey.GetModulus().Encode(&modulusBinary[0],modulusBinary.size());
	inputKey.GetPublicExponent().Encode(&exponentBinary[0],exponentBinary.size());

	ByteBuffer theMessage;
	theMessage.append(modulusBinary);
	theMessage.append(exponentBinary);

	return theMessage;
}

void AuthServer::GenerateCryptoKeys( string &privKeyOut, string &pubKeyOut )
{
	CryptoPP::RSA::PublicKey publicKey;
	CryptoPP::RSA::PrivateKey privateKey;
	GenerateRSAKeys(1024,publicKey,privateKey);

	//just dump private key to file so we can load it
	CryptoPP::StringSink privOutput(privKeyOut);
	privateKey.DEREncodePrivateKey(privOutput);
	privOutput.MessageEnd();

	//for public key, we have to follow mxo standard
	CryptoPP::StringSink pubOutput(pubKeyOut);
	pubOutput.PutWord32(4,CryptoPP::BIG_ENDIAN_ORDER);
	publicKey.GetModulus().DEREncode(pubOutput);
	publicKey.GetPublicExponent().DEREncode(pubOutput);
	//separates pubkey from signature
	pubOutput.Put(0);

	//generate signature
	ByteBuffer signMe = MessageFromPublicKey(publicKey);
	vector<byte> signature;
	signature.resize(signer2048bit->MaxSignatureLength());
	size_t actualSignatureSize = signer2048bit->SignMessage(randPool,(byte*)signMe.contents(),signMe.size(),&signature[0]);
	signature.resize(actualSignatureSize);

	//cache for later retrieval
	pubKeySignature = signature;

	//append signature to file
	for (size_t i=0;i<signature.size();i++)
	{
		pubOutput.Put(signature[i]);
	}
	pubOutput.MessageEnd();
}

void AuthServer::LoadCryptoKeys()
{
	if (signer2048bit.get() == NULL || verifier2048bit.get() == NULL)
	{
		LoadSignKeys();
	}

	bool invalidKeys = false;

	ifstream f_privateKey;
	f_privateKey.open("privkey.dat",ios::binary);
	if (f_privateKey.is_open())
	{
		f_privateKey.seekg(0,ios::end);
		ifstream::pos_type privKeySize = f_privateKey.tellg();
		vector<byte> privKeyStorage;
		privKeyStorage.resize(privKeySize);
		f_privateKey.seekg(0,ios::beg);
		f_privateKey.read((char*)&privKeyStorage[0],privKeyStorage.size());
		f_privateKey.close();

		string pkeyStr = string((const char*)&privKeyStorage[0],privKeyStorage.size());

		CryptoPP::InvertibleRSAFunction params;
		CryptoPP::StringSource pkeySource(pkeyStr,true);
		params.BERDecodePrivateKey(pkeySource,false,pkeySource.MaxRetrievable());

		CryptoPP::RSA::PrivateKey privateKey( params );
		CryptoPP::RSA::PublicKey publicKey( params );

		ifstream f_pubKey;
		f_pubKey.open("pubkey.dat",ios::binary);
		if (f_pubKey.is_open())
		{
			f_pubKey.seekg(0,ios::end);
			ifstream::pos_type pubKeySize = f_pubKey.tellg();
			vector<byte> pubKeyStorage;
			pubKeyStorage.resize(pubKeySize);
			f_pubKey.seekg(0,ios::beg);
			f_pubKey.read((char*)&pubKeyStorage[0],pubKeyStorage.size());
			f_pubKey.close();	

			ByteBuffer pubKeyBuf(pubKeyStorage);

			uint32 rsaMethod;
			pubKeyBuf >> rsaMethod;
			rsaMethod = swap32(rsaMethod);

			if (rsaMethod != 4)
			{
				invalidKeys = true;
			}
			else
			{
				vector <byte> derEncodedPubKey;
				derEncodedPubKey.resize( pubKeyBuf.size() - pubKeyBuf.rpos() - sizeof(uint8) - verifier2048bit->MaxSignatureLength() );
				pubKeyBuf.read(&derEncodedPubKey[0],derEncodedPubKey.size());
				uint8 zeroSeparator;
				pubKeyBuf >> zeroSeparator;

				if (zeroSeparator != 0)
				{
					invalidKeys = true;
				}
				else
				{
					vector<byte> signature;
					signature.resize(verifier2048bit->MaxSignatureLength());
					pubKeyBuf.read(&signature[0],signature.size());

					string pubKeyString = string((const char*)&derEncodedPubKey[0],derEncodedPubKey.size());
					CryptoPP::StringSource pubKeySource(pubKeyString,true);

					CryptoPP::Integer Modulus;
					Modulus.BERDecode(pubKeySource);
					CryptoPP::Integer Exponent;
					Exponent.BERDecode(pubKeySource);

					if (Modulus != publicKey.GetModulus() || Exponent != publicKey.GetPublicExponent())
					{
						invalidKeys = true;
					}
					else
					{
						ByteBuffer verifyMe = MessageFromPublicKey(publicKey);

						bool messageCorrect = verifier2048bit->VerifyMessage(
							(byte*)verifyMe.contents(),
							verifyMe.size(),
							&signature[0],
							signature.size());

						if (messageCorrect == true)
						{
							//cache this signature
							pubKeySignature = signature;
							invalidKeys = false;
						}
						else
						{
							invalidKeys = true;
						}
					}
				}

			}
		}
		else
		{
			invalidKeys = true;
		}

		if (invalidKeys == false)
		{
			rsaDecryptor = auto_ptr<CryptoPP::RSAES_OAEP_SHA_Decryptor>(new CryptoPP::RSAES_OAEP_SHA_Decryptor(privateKey));
			rsaEncryptor = auto_ptr<CryptoPP::RSAES_OAEP_SHA_Encryptor>(new CryptoPP::RSAES_OAEP_SHA_Encryptor(publicKey));
			signer1024bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer(privateKey));
			verifier1024bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier(publicKey));
			pubKeyModulus = publicKey.GetModulus();
		}
	}
	else
	{
		invalidKeys = true;
	}

	if (invalidKeys == true)
	{
		//generate new rsa key
		string outPrivate, outPublic;
		GenerateCryptoKeys(outPrivate,outPublic);
		ofstream fileStream;
		if (outPrivate.size() > 0)
		{
			//initialize RSA engine
			CryptoPP::InvertibleRSAFunction params;
			CryptoPP::StringSource pkeySource(outPrivate,true);
			params.BERDecodePrivateKey(pkeySource,false,pkeySource.MaxRetrievable());
			CryptoPP::RSA::PrivateKey privateKey( params );
			CryptoPP::RSA::PublicKey publicKey( params );

			rsaDecryptor = auto_ptr<CryptoPP::RSAES_OAEP_SHA_Decryptor>(new CryptoPP::RSAES_OAEP_SHA_Decryptor(privateKey));
			rsaEncryptor = auto_ptr<CryptoPP::RSAES_OAEP_SHA_Encryptor>(new CryptoPP::RSAES_OAEP_SHA_Encryptor(publicKey));
			signer1024bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Signer(privateKey));
			verifier1024bit = auto_ptr<CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier>(new CryptoPP::Weak::RSASSA_PKCS1v15_MD5_Verifier(publicKey));
			pubKeyModulus = publicKey.GetModulus();

			//write to file
			fileStream.open("privkey.dat",ios::binary | ios::trunc);
			fileStream.write(outPrivate.data(),outPrivate.size());
			fileStream.close();
		}
		if (outPublic.size() > 0)
		{
			//write to file, mxo client will use this
			fileStream.open("pubkey.dat",ios::binary | ios::trunc);
			fileStream.write(outPublic.data(),outPublic.size());
			fileStream.close();
		}
	}
}

string AuthServer::Encrypt(string input)
{
	string output;
	CryptoPP::StringSource(input,true, new CryptoPP::PK_EncryptorFilter(randPool, *rsaEncryptor, new CryptoPP::StringSink(output)));
	return output;
}

string AuthServer::Decrypt(string input)
{
	string output;
	CryptoPP::StringSource(input,true, new CryptoPP::PK_DecryptorFilter(randPool, *rsaDecryptor, new CryptoPP::StringSink(output)));
	return output;
}

ByteBuffer AuthServer::SignWith1024Bit( byte *message,size_t messageLen )
{
	//generate signature
	ByteBuffer signMe(message,messageLen);
	vector<byte> signature;
	signature.resize(signer1024bit->MaxSignatureLength());
	size_t actualSignatureSize = signer1024bit->SignMessage(randPool,(byte*)signMe.contents(),signMe.size(),&signature[0]);
	signature.resize(actualSignatureSize);

	return ByteBuffer(signature);
}

bool AuthServer::VerifyWith1024Bit( byte *message,size_t messageLen,byte *signature,size_t signatureLen )
{
	return verifier1024bit->VerifyMessage(message,messageLen,signature,signatureLen);
}

ByteBuffer AuthServer::GetPubKeyData()
{
	ByteBuffer result;

	string pubKeyModulusStr;
	CryptoPP::StringSink pubKeyModulusSink(pubKeyModulusStr);
	pubKeyModulus.Encode(pubKeyModulusSink,pubKeyModulus.MinEncodedSize());

	result << uint16(pubKeyModulusStr.size());
	result.append(pubKeyModulusStr);

	result << uint16(pubKeySignature.size());
	result.append(pubKeySignature);

	return result;
}

AuthServer::AuthServer()
{
	listenSocketInst = NULL;
}

AuthServer::~AuthServer()
{
	if (listenSocketInst != NULL)
	{
		delete listenSocketInst;
	}
}

void AuthServer::Start()
{
	LoadCryptoKeys();

	int Port = sConfig.GetIntDefault("AuthServer.Port",11000);
	INFO_LOG("Starting Auth server on port %d", Port);	

	if (listenSocketInst != NULL)
	{
		delete listenSocketInst;
	}
	listenSocketInst = new AuthListenSocket(authSocketHandler);
	listenSocketInst->Bind(Port);
	authSocketHandler.Add(listenSocketInst);
}

void AuthServer::Stop()
{
	INFO_LOG("Auth Server shutdown");
	if (listenSocketInst != NULL)
	{
		delete listenSocketInst;
		listenSocketInst = NULL;
	}
}


void AuthServer::Loop(void)
{
	authSocketHandler.Select(0, 100000);                      // 100 ms
}