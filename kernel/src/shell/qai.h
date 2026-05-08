#pragma once

// ---------------------------------------------------------------------------
//  shell/qai.h — Quanta AI (QAI) built-in assistant
//
//  A compiled-in, network-free knowledge engine.
//  Answers questions about Quanta OS, OS concepts, and system status
//  using keyword matching + a curated knowledge base.
// ---------------------------------------------------------------------------

// Answer a question and print the response to the shell output.
void qai_answer(const char *question);
