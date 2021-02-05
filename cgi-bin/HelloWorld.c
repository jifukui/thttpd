#include <stdio.h>
#include <dlfcn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
int main(){
    struct SSL_CTX *jifukuictx;
    struct SSL *ssl;
    int ret;
    int ssln ;
    SSL_load_error_strings();
    SSL_library_init();
    jifukuictx = SSL_CTX_new(SSLv23_server_method());
    //jifukuictx = SSL_CTX_new(TLSv1_2_server_method());
	if (jifukuictx == NULL)
	{
		//printf("load method over\r\n");
	}
	else if (SSL_CTX_use_certificate_file(jifukuictx, "thttpd.pem", SSL_FILETYPE_PEM) == 0)
	{
		//printf("cannot open certificate\r\n");
	}
	else if (SSL_CTX_use_PrivateKey_file(jifukuictx, "thttpd.pem", SSL_FILETYPE_PEM) == 0)
	{
		//printf("cannot open PrivateKey\r\n");
	}else{
		//printf("good for openssl\r\n");
	}
    ssl = SSL_new(jifukuictx);
    if(ssl){
        //printf("creat ssl success\r\n");
    }else{
        //printf("creat ssl failed\r\n");
    }
    SSL_set_fd(ssl,0);
    ssln = SSL_accept(ssl);
    if(ssln){
        SSL_write(ssl,"hello jifukui",14);
    }
    return 0;
}