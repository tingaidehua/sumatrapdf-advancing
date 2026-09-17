/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

void ShowPdfBakeDialog(MainWindow* win);
void ShowPdfExtractTextDialog(MainWindow* win);
void ShowPdfCompressDialog(MainWindow* win);
// Compress an on-disk PDF (library / NotebookLM). Default dest: <name>-压缩.pdf
// If continueNotebookLmAdd, after success shows size then queues NotebookLM upload of the result.
void ShowPdfCompressDialogForPath(MainWindow* win, Str pdfPath, bool openAfter = false,
                                  bool continueNotebookLmAdd = false, i64 notebookLmBookId = 0,
                                  Str notebookLmTitle = {});
// Same-folder sibling: <pathNoExt>-压缩.pdf
TempStr CompressedPdfSiblingPathTemp(Str pdfPath);
constexpr i64 kNotebookLmMaxUploadBytes = 200LL * 1024 * 1024;
void ShowPdfDecompressDialog(MainWindow* win);
void ShowPdfDeletePageDialog(MainWindow* win);
void ShowPdfExtractPagesDialog(MainWindow* win);
void ShowPdfEncryptDialog(MainWindow* win);
void ShowPdfDecryptDialog(MainWindow* win);
// comic books / image folders / single images → multi-page PDF (issue #4118)
void ShowConvertToPdfDialog(MainWindow* win);
// PDF pages → PNG / JPEG / BMP files (issue #5991)
void ShowConvertPdfToImagesDialog(MainWindow* win);
TempStr ConvertPagesToImagesResultTemp(Str templatePath, Str pagesSpec, int* exitCodeOut);
