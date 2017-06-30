# GoogleAuthApache - Module to implement Two-Factor-Authentication in Apache2

BUILDING:

You have to have the Apache development environment on your system. This uses the "apxs" script, which should be built and installed as a part of the Apache development environment.

LIMITATIONS:

Does not use scratch codes

Does not invalidate codes after use. 


DISCUSSION:

Code reuse is tricky. Because HTTP requests, unlike interactive SSH sessions, are very short lived. It is therefore very likely that a code would need to be re-used immediatley to request another page.

Also, the way HTTP works, is that once validated, the browser keeps sending the same credentials over and over. As our codes expire outside of a 60 second window - that means after 60 seconds of interacting with a web server, you will be asked to supply a new key. There are two possible fixes for this: 1. Make the keys valid for much longer periods of time 2. Write an authetication cookie to the users's browser, and work with that after the first request. 

Due to the way Apache handles Digest authentication, there is no leeway in time allowed. With Basic authentication, the module can try times around the current time for a valid key, but only one attempt is made in Digest mode. Thus, this requires a high level of clock synchronization between clients and servers.


RECOMENDATION:

Someone can always sniff the wire and steal your cookies to hijack your session. This cannot be done if you are using SSL, which is highly recomended - and imperitive for any "real" security.

Configuration:

GoogleAuthPath is the root directory to hold user authentication, or secret key files in. This can be specified as either a path relative to Apache's root, or an absolute path. For example, if I specified "ga_auth" (as below) and I logged in with the username "user" - the module would look for a file called: /usr/local/apache2/ga_auth/user (under my setup) for the user's login credentials. See "Secret Key Files" below for more info.

GoogleAuthCookieLife specifies how long (in seconds) authentication cookies are to last. Not specifying this disables authentication cookies, meaning "sessions" will only last about a minute before needed re-authentication.

EntryWindow specifies the "leeway" in how exact the time has to be for a proper authentication to occur. "0" would mean it must be exact. "1" would mean it will except one entry newer or older. (About +/- 30 seconds) "2" would accept entries +/- 60 seconds. The default value is "1". Higher numbers can be used to account for clocks not being entirely accurate, but are not as secure. This is only used for Basic authentication, as Digest authentication will only work with the extact code required at the exact present time.

Example Configuration:

Basic Authentication:

The Basic-Authenticaiton-specific below are that: * AuthBasicProvider is set to google_authenticator * AuthType is Basic

```

Loadmodule authn_google_module modules/mod_authn_google.so Options FollowSymLinks Indexes ExecCGI AllowOverride All Order deny,allow Allow from all

AuthType Basic 
AuthName "My Test" 
AuthBasicProvider "google_authenticator" 
Require valid-user 
GoogleAuthUserPath ga_auth 
GoogleAuthCookieLife 3600 
GoogleAuthEntryWindow 2

Unknown end tag for </Directory>

```

Digest Authentication:

The Digest-Authenticaiton-specific below are that: * AuthDigestProvider is set to google_authenticator * AuthType is Digest * AuthDigestDomain is set to something. (See Apache Docs)

```

Loadmodule authn_google_module modules/mod_authn_google.so Options FollowSymLinks Indexes ExecCGI AllowOverride All Order deny,allow Allow from all

AuthType Digest 
AuthDigestDomain /private/ http://mirror.example.com/private2/ 
AuthDigestProvider "google_authenticator"
AuthName "My Test"
Require valid-user 
GoogleAuthUserPath ga_auth 
GoogleAuthCookieLife 3600 
GoogleAuthEntryWindow 1

Unknown end tag for </Directory>

```

Secret Key File:

This file may be generated from the "google_authenticator" program, consisting of the secret key, scratch codes, etc.

You can alternativley create your own. It shall consist of a single line containing the users' 16-digit Base32-encoded key (which would be also entered into the Google Authentication application on the users' device).

To add a second factor to the authentication (i.e. a password), add a line in the user's authentication password such as:

"PASSWORD=mySecret

Note that this line starts with a double-quote, which is the convention to make it look like a comment in a standard Google Authenticator file.

So a complete file, including a password may look like this:

abcdefabcdef2345 
"PASSWORD=mySecret

When a static password is used in conjunction with the secret key, the user should enter the static password immediately followed by the 6-digit one-time code when prompted for a password. For example, in the example above, when the user was prompted to login, they would check their Google Authenticator application and receive a one-time code such as 123456. In such a case, they would enter a password as:

mySecret123456

Troubleshooting:

Obviously, check your Apache log file. One very important thing is to make sure you have proper time synchronization. Use of a service such as NTP is highly recommended. Using a larger "AuthEntryWindow" (above) can help compensate for slop in time sync.
