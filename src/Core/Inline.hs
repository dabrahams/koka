-----------------------------------------------------------------------------
-- Copyright 2020 Microsoft Corporation, Daan Leijen, Ningning Xie
--
-- This is free software; you can redistribute it and/or modify it under the
-- terms of the Apache License, Version 2.0. A copy of the License can be
-- found in the file "license.txt" at the root of this distribution.
-----------------------------------------------------------------------------

-----------------------------------------------------------------------------
-- Inl all local and anonymous functions to top level. No more letrec :-)
-----------------------------------------------------------------------------

module Core.Inline( inlineDefs
                   ) where


import qualified Lib.Trace
import Control.Monad
import Control.Applicative

import Lib.PPrint
import Common.Failure
import Common.NamePrim ( nameEffectOpen )
import Common.Name
import Common.Range
import Common.Unique
import Common.Error
import Common.Syntax

import Kind.Kind
import Type.Type
import Type.Kind
import Type.TypeVar
import Type.Pretty hiding (Env)
import qualified Type.Pretty as Pretty
import Type.Assumption
import Core.Core
import qualified Core.Core as Core
import Core.Pretty
import Core.Inlines

trace s x =
  Lib.Trace.trace s
    x



inlineDefs :: Pretty.Env -> Int -> Inlines -> DefGroups -> (DefGroups,Int)
inlineDefs penv u inlines defs
  = runInl penv u inlines $
    do traceDoc $ \penv -> ppInlines penv inlines
       inlDefGroups defs


{--------------------------------------------------------------------------
  transform definition groups
--------------------------------------------------------------------------}
inlDefGroups :: DefGroups -> Inl DefGroups
inlDefGroups defGroups
  = do traceDoc (\penv -> text "inlining")
       mapM inlDefGroup defGroups

inlDefGroup (DefRec defs)
  = do defs' <- mapM inlDef defs
       return (DefRec defs')
inlDefGroup (DefNonRec def)
 = do def' <- inlDef def
      return (DefNonRec def')

inlDef :: Def -> Inl Def
inlDef def
  = withCurrentDef def $
    do expr' <- inlExpr (defExpr def)
       return def{ defExpr = expr' }

inlExpr :: Expr -> Inl Expr
inlExpr expr
  = case expr of
    -- Applications
    App (TypeApp f targs) args
      -> do f' <- inlAppExpr f (length targs) (argLength args)
            args' <- mapM inlExpr args
            return (App f' args')

    App f args
      -> do args' <- mapM inlExpr args
            f' <- inlAppExpr f 0 (argLength args)
            return (App f' args')

    -- regular cases
    Lam args eff body
      -> do -- inlTraceDoc $ \env -> text "not effectful lambda:" <+> niceType env eff
            body' <- inlExpr body
            return (Lam args eff body')

    Let defgs body
      -> do defgs' <- inlDefGroups defgs
            body'  <- inlExpr body
            return (Let defgs' body')
    Case exprs bs
      -> do exprs' <- mapM inlExpr exprs
            bs'    <- mapM inlBranch bs
            return (Case exprs' bs')

    -- type application and abstraction
    TypeLam tvars body
      -> do body' <- inlExpr body
            return $ TypeLam tvars body'

    TypeApp body tps
      -> do body' <- inlAppExpr body (length tps) 0
            return $ TypeApp body' tps

    -- the rest
    _ -> inlAppExpr expr 0 0
 where
   argLength args
     = if (all isVar args) then 0   -- prevent inlining of functions with just variable argmuments
        else length args

   isVar (Var _ _) = True
   isVar _         = False

inlAppExpr :: Expr -> Int -> Int -> Inl Expr
inlAppExpr expr m n
  = case expr of
      App eopen@(TypeApp (Var open info) targs) [f] | getName open == nameEffectOpen
        -> do f' <- inlAppExpr f m n
              return (App eopen [f'])
      Var tname varInfo
        -> do mbInfo <- inlLookup (getName tname)
              case mbInfo of
                Just (info,m',n') | not (inlineRec info) && (m >= m') && (n >= n')
                  -> do traceDoc $ \penv -> text "inlined:" <+> ppName penv (getName tname)
                        return (inlineExpr info)
                Just (info,m',n')
                  -> do traceDoc $ \penv -> text "inline candidate:" <+> ppName penv (getName tname) <+> text (show (m',n')) <+> text "vs" <+> text (show (m,n))
                        return expr
                Nothing -> do traceDoc $ \penv -> text "not inline candidate:" <+> ppName penv (getName tname)
                              return expr
      _ -> return expr  -- no inlining


inlBranch :: Branch -> Inl Branch
inlBranch (Branch pat guards)
  = do guards' <- mapM inlGuard guards
       return $ Branch pat guards'

inlGuard :: Guard -> Inl Guard
inlGuard (Guard guard body)
  = do guard' <- inlExpr guard
       body'  <- inlExpr body
       return $ Guard guard' body'


{--------------------------------------------------------------------------
  Inl inlad
--------------------------------------------------------------------------}
newtype Inl a = Inl (Env -> State -> Result a)

data Env = Env{ currentDef :: [Def],
                prettyEnv :: Pretty.Env,
                inlines :: Inlines }

data State = State{ uniq :: Int }

data Result a = Ok a State

runInl :: Pretty.Env -> Int -> Inlines -> Inl a -> (a,Int)
runInl penv u inlines (Inl c)
  = case c (Env [] penv inlines) (State u) of
      Ok x st -> (x,uniq st)

instance Functor Inl where
  fmap f (Inl c)  = Inl (\env st -> case c env st of
                                        Ok x st' -> Ok (f x) st')

instance Applicative Inl where
  pure  = return
  (<*>) = ap

instance Monad Inl where
  return x       = Inl (\env st -> Ok x st)
  (Inl c) >>= f = Inl (\env st -> case c env st of
                                      Ok x st' -> case f x of
                                                    Inl d -> d env st' )

instance HasUnique Inl where
  updateUnique f = Inl (\env st -> Ok (uniq st) st{ uniq = (f (uniq st)) })
  setUnique  i   = Inl (\env st -> Ok () st{ uniq = i} )

withEnv :: (Env -> Env) -> Inl a -> Inl a
withEnv f (Inl c)
  = Inl (\env st -> c (f env) st)

getEnv :: Inl Env
getEnv
  = Inl (\env st -> Ok env st)

updateSt :: (State -> State) -> Inl State
updateSt f
  = Inl (\env st -> Ok st (f st))

withCurrentDef :: Def -> Inl a -> Inl a
withCurrentDef def action
  = -- trace ("inl def: " ++ show (defName def)) $
    withEnv (\env -> env{currentDef = def:currentDef env}) $
    action

inlLookup :: Name -> Inl (Maybe (InlineDef,Int,Int))
inlLookup name
  = do env <- getEnv
       case (inlinesLookup name (inlines env)) of
         Nothing   -> return Nothing
         Just idef -> let (m,n) = getArity (inlineExpr idef)
                      in return (Just (idef,m,n))

  where
    getArity expr
      = case expr of
          TypeLam tpars (Lam args eff body) -> (length tpars, length args)
          TypeLam tpars body                -> (length tpars, 0)
          Lam args eff body                 -> (0, length args)
          _                                 -> (0,0)

traceDoc :: (Pretty.Env -> Doc) -> Inl ()
traceDoc f
  = do env <- getEnv
       inlTrace (show (f (prettyEnv env)))

inlTrace :: String -> Inl ()
inlTrace msg
  = do env <- getEnv
       trace ("inl: " ++ show (map defName (currentDef env)) ++ ": " ++ msg) $ return ()
