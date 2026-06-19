# ChromeKit #

ChromeKit is a PoC used to show how threat actors **could** bypass Chromes ABE(app bound encryption) without administrator privileges.

It isn't currently finished and i still need to work on it.

This tool is for learning porpuses and **not** to cause damage or steal personal information.

# How does it work?

1. ChromeKit first starts by spawning a new Chrome process.

2. It then hides the process, via EnumWindows and ShowWindow with the SW_HIDE flag, it takes a few seconds for Chromes window to pop up, so ChromeKit sleeps for 2000 milliseconds.

3. It gets the base address of chrome.dll cause that is where most of Chrome lives, chrome.exe is mostly a shell or a launcher just to load chrome.dll

4. We validate the NT and DoS signature/header

5. We get the amounts and names of sections via the nt header.

6. We search specifically for the address of the .rdata and .text section.

7. After we find the .rdata and .text section, we search it for the string "OSCrypt.AppBoundProvider.Decrypt.ResultCode"

8. If we find it in the .rdata section, we set found to true so we don't need to go throug the .text search(it mostly is in the .rdata section i dont know why i included the .text search)