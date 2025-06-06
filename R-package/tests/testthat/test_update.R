context("update trees in an existing model")

data(agaricus.train, package = 'xgboost')
data(agaricus.test, package = 'xgboost')

n_threads <- 1

dtrain <- xgb.DMatrix(
  agaricus.train$data, label = agaricus.train$label, nthread = n_threads
)
dtest <- xgb.DMatrix(
  agaricus.test$data, label = agaricus.test$label, nthread = n_threads
)

# Disable flaky tests for 32-bit Windows.
# See https://github.com/dmlc/xgboost/issues/3720
win32_flag <- .Platform$OS.type == "windows" && .Machine$sizeof.pointer != 8

test_that("updating the model works", {
  evals <- list(train = dtrain, test = dtest)

  # no-subsampling
  p1 <- xgb.params(
    objective = "binary:logistic",
    max_depth = 2,
    learning_rate = 0.05,
    nthread = n_threads,
    updater = "grow_colmaker,prune"
  )
  set.seed(11)
  bst1 <- xgb.train(p1, dtrain, nrounds = 10, evals = evals, verbose = 0)
  tr1 <- xgb.model.dt.tree(model = bst1)

  # with subsampling
  p2 <- modifyList(p1, list(subsample = 0.1))
  set.seed(11)
  bst2 <- xgb.train(p2, dtrain, nrounds = 10, evals = evals, verbose = 0)
  tr2 <- xgb.model.dt.tree(model = bst2)

  # the same no-subsampling boosting with an extra 'refresh' updater:
  p1r <- modifyList(p1, list(updater = 'grow_colmaker,prune,refresh', refresh_leaf = FALSE))
  set.seed(11)
  bst1r <- xgb.train(p1r, dtrain, nrounds = 10, evals = evals, verbose = 0)
  tr1r <- xgb.model.dt.tree(model = bst1r)
  # all should be the same when no subsampling
  expect_equal(attributes(bst1)$evaluation_log, attributes(bst1r)$evaluation_log)
  expect_equal(
    jsonlite::fromJSON(rawToChar(xgb.save.raw(bst1, raw_format = "json"))),
    jsonlite::fromJSON(rawToChar(xgb.save.raw(bst1r, raw_format = "json"))),
    tolerance = 1e-6
  )
  if (!win32_flag) {
    expect_equal(tr1, tr1r, tolerance = 0.00001, check.attributes = FALSE)
  }

  # the same boosting with subsampling with an extra 'refresh' updater:
  p2r <- modifyList(p2, list(updater = 'grow_colmaker,prune,refresh', refresh_leaf = FALSE))
  set.seed(11)
  bst2r <- xgb.train(p2r, dtrain, nrounds = 10, evals = evals, verbose = 0)
  tr2r <- xgb.model.dt.tree(model = bst2r)
  # should be the same evaluation but different gains and larger cover
  expect_equal(attributes(bst2)$evaluation_log, attributes(bst2r)$evaluation_log)
  if (!win32_flag) {
    expect_equal(tr2[Feature == 'Leaf']$Gain, tr2r[Feature == 'Leaf']$Gain)
  }
  expect_gt(sum(abs(tr2[Feature != 'Leaf']$Gain - tr2r[Feature != 'Leaf']$Gain)), 100)
  expect_gt(sum(tr2r$Cover) / sum(tr2$Cover), 1.5)

  # process type 'update' for no-subsampling model, refreshing the tree stats AND leaves from training data:
  set.seed(123)
  p1u <- modifyList(p1, list(process_type = 'update', updater = 'refresh', refresh_leaf = TRUE))
  bst1u <- xgb.train(p1u, dtrain, nrounds = 10, evals = evals, verbose = 0, xgb_model = bst1)
  tr1u <- xgb.model.dt.tree(model = bst1u)
  # all should be the same when no subsampling
  expect_equal(attributes(bst1)$evaluation_log, attributes(bst1u)$evaluation_log)
  expect_equal(
    jsonlite::fromJSON(rawToChar(xgb.save.raw(bst1, raw_format = "json"))),
    jsonlite::fromJSON(rawToChar(xgb.save.raw(bst1u, raw_format = "json"))),
    tolerance = 1e-6
  )
  expect_equal(tr1, tr1u, tolerance = 0.00001, check.attributes = FALSE)

  # same thing but with a serialized model
  set.seed(123)
  bst1u <- xgb.train(p1u, dtrain, nrounds = 10, evals = evals, verbose = 0, xgb_model = xgb.save.raw(bst1))
  tr1u <- xgb.model.dt.tree(model = bst1u)
  # all should be the same when no subsampling
  expect_equal(attributes(bst1)$evaluation_log, attributes(bst1u)$evaluation_log)
  expect_equal(tr1, tr1u, tolerance = 0.00001, check.attributes = FALSE)

  # process type 'update' for model with subsampling, refreshing only the tree stats from training data:
  p2u <- modifyList(p2, list(process_type = 'update', updater = 'refresh', refresh_leaf = FALSE))
  bst2u <- xgb.train(p2u, dtrain, nrounds = 10, evals = evals, verbose = 0, xgb_model = bst2)
  tr2u <- xgb.model.dt.tree(model = bst2u)
  # should be the same evaluation but different gains and larger cover
  expect_equal(attributes(bst2)$evaluation_log, attributes(bst2u)$evaluation_log)
  expect_equal(tr2[Feature == 'Leaf']$Gain, tr2u[Feature == 'Leaf']$Gain)
  expect_gt(sum(abs(tr2[Feature != 'Leaf']$Gain - tr2u[Feature != 'Leaf']$Gain)), 100)
  expect_gt(sum(tr2u$Cover) / sum(tr2$Cover), 1.5)
  # the results should be the same as for the model with an extra 'refresh' updater
  expect_equal(attributes(bst2r)$evaluation_log, attributes(bst2u)$evaluation_log)
  if (!win32_flag) {
    expect_equal(tr2r, tr2u, tolerance = 0.00001, check.attributes = FALSE)
  }

  # process type 'update' for no-subsampling model, refreshing only the tree stats from TEST data:
  p1ut <- modifyList(p1, list(process_type = 'update', updater = 'refresh', refresh_leaf = FALSE))
  bst1ut <- xgb.train(p1ut, dtest, nrounds = 10, evals = evals, verbose = 0, xgb_model = bst1)
  tr1ut <- xgb.model.dt.tree(model = bst1ut)
  # should be the same evaluations but different gains and smaller cover (test data is smaller)
  expect_equal(attributes(bst1)$evaluation_log, attributes(bst1ut)$evaluation_log)
  expect_equal(tr1[Feature == 'Leaf']$Gain, tr1ut[Feature == 'Leaf']$Gain)
  expect_gt(sum(abs(tr1[Feature != 'Leaf']$Gain - tr1ut[Feature != 'Leaf']$Gain)), 100)
  expect_lt(sum(tr1ut$Cover) / sum(tr1$Cover), 0.5)
})

test_that("updating works for multiclass & multitree", {
  dtr <- xgb.DMatrix(
    as.matrix(iris[, -5]), label = as.numeric(iris$Species) - 1, nthread = n_threads
  )
  evals <- list(train = dtr)
  p0 <- xgb.params(
    max_depth = 2,
    learning_rate = 0.5,
    nthread = n_threads,
    subsample = 0.6,
    objective = "multi:softprob",
    num_class = 3,
    num_parallel_tree = 2,
    base_score = 0
  )
  set.seed(121)
  bst0 <- xgb.train(p0, dtr, 5, evals = evals, verbose = 0)
  tr0 <- xgb.model.dt.tree(model = bst0)

  # run update process for an original model with subsampling
  p0u <- modifyList(p0, list(process_type = 'update', updater = 'refresh', refresh_leaf = FALSE))
  bst0u <- xgb.train(p0u, dtr, nrounds = xgb.get.num.boosted.rounds(bst0),
                     evals = evals, xgb_model = bst0, verbose = 0)
  tr0u <- xgb.model.dt.tree(model = bst0u)

  # should be the same evaluation but different gains and larger cover
  expect_equal(attributes(bst0)$evaluation_log, attributes(bst0u)$evaluation_log)
  expect_equal(tr0[Feature == 'Leaf']$Gain, tr0u[Feature == 'Leaf']$Gain)
  expect_gt(sum(abs(tr0[Feature != 'Leaf']$Gain - tr0u[Feature != 'Leaf']$Gain)), 100)
  expect_gt(sum(tr0u$Cover) / sum(tr0$Cover), 1.5)
})
