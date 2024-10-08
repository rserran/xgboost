% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/callbacks.R
\name{xgb.cb.print.evaluation}
\alias{xgb.cb.print.evaluation}
\title{Callback for printing the result of evaluation}
\usage{
xgb.cb.print.evaluation(period = 1, showsd = TRUE)
}
\arguments{
\item{period}{Results would be printed every number of periods.}

\item{showsd}{Whether standard deviations should be printed (when available).}
}
\value{
An \code{xgb.Callback} object, which can be passed to \code{\link[=xgb.train]{xgb.train()}} or \code{\link[=xgb.cv]{xgb.cv()}}.
}
\description{
The callback function prints the result of evaluation at every \code{period} iterations.
The initial and the last iteration's evaluations are always printed.

Does not leave any attribute in the booster (see \link{xgb.cb.evaluation.log} for that).
}
\seealso{
\link{xgb.Callback}
}
