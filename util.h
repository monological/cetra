#ifndef _UTIL_H_
#define _UTIL_H_


void CheckOpenGLError(const char* where);
char* convert_windows_path_to_unix(const char* windows_path);
char* convert_and_normalize_path(const char *input_path);


#endif // _UTIL_H_

