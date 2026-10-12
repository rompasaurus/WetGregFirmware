/**
 * i18n.h — UI string table: English literal → Egyptian Arabic (UTF-8).
 *
 * Purpose: let every existing draw_text("...") site show Arabic without
 * touching it. text_ar.c looks each literal up here when the language is
 * Arabic; a miss leaves the English (Latin font) as is. "KEY: VALUE" strings
 * are looked up piecewise by the caller, so only the parts need entries.
 */
#ifndef I18N_H
#define I18N_H

/* The Arabic for an English UI string, or NULL when there is no entry. */
const char *i18n_lookup(const char *en);

/* The intro story beats (same count/order as screens_anim.c's English). */
const char *i18n_intro_line(int i);

#endif /* I18N_H */
