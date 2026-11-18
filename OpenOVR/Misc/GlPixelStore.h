#pragma once
// The current OpenGL context's pixel-store state set aside while tightly packed rows are read from a texture into
// memory (pack) or written from memory into one (unpack), and put back afterwards: the alignment, the row length and
// skips, and a bound pixel buffer object, which would take the transfer in place of the memory. A game may have left
// any of these set.
//
// Include after the OpenGL header (and GLX's on Linux). The caller clears the context's earlier errors first.

class GlPixelStore {
public:
	explicit GlPixelStore(bool pack)
	    : alignmentName(pack ? 0x0D05 : 0x0CF5), rowLengthName(pack ? 0x0D02 : 0x0CF2), skipRowsName(pack ? 0x0D03 : 0x0CF3),
	      skipPixelsName(pack ? 0x0D04 : 0x0CF4), bufferTarget(pack ? 0x88EB : 0x88EC)
	{
		glGetIntegerv(alignmentName, &alignment);
		glGetIntegerv(rowLengthName, &rowLength);
		glGetIntegerv(skipRowsName, &skipRows);
		glGetIntegerv(skipPixelsName, &skipPixels);
		// GL_PIXEL_PACK_BUFFER_BINDING or GL_PIXEL_UNPACK_BUFFER_BINDING. A context older than OpenGL 2.1 has no pixel
		// buffer objects: the query fails there, and none is bound.
		glGetIntegerv(pack ? 0x88ED : 0x88EF, &buffer);
		if (glGetError() != GL_NO_ERROR)
			buffer = 0;
		if (buffer) {
#ifdef _WIN32
			bindBuffer = (BindBuffer)wglGetProcAddress("glBindBuffer");
#else
			bindBuffer = (BindBuffer)(void*)glXGetProcAddress((const GLubyte*)"glBindBuffer");
#endif
			if (!bindBuffer)
				return;
			bindBuffer(bufferTarget, 0);
		}
		glPixelStorei(alignmentName, 1);
		glPixelStorei(rowLengthName, 0);
		glPixelStorei(skipRowsName, 0);
		glPixelStorei(skipPixelsName, 0);
		ok = true;
	}

	~GlPixelStore()
	{
		if (!ok)
			return;
		glPixelStorei(alignmentName, alignment);
		glPixelStorei(rowLengthName, rowLength);
		glPixelStorei(skipRowsName, skipRows);
		glPixelStorei(skipPixelsName, skipPixels);
		if (buffer)
			bindBuffer(bufferTarget, (GLuint)buffer);
	}

	GlPixelStore(const GlPixelStore&) = delete;
	GlPixelStore& operator=(const GlPixelStore&) = delete;

	// False when a pixel buffer object is bound and can't be unbound: nothing was changed, and the transfer must not
	// be made
	bool Ok() const { return ok; }

private:
#ifdef _WIN32
	using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
#else
	using BindBuffer = void (*)(GLenum, GLuint);
#endif
	const GLenum alignmentName, rowLengthName, skipRowsName, skipPixelsName, bufferTarget;
	GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0, buffer = 0;
	BindBuffer bindBuffer = nullptr;
	bool ok = false;
};
